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
 * Copyright (C) 2012 Google Inc.
 */

#include <config.h>

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <ctype.h>

#include "mm-broadband-modem-qmi.h"

#include "ModemManager.h"
#include "mm-log.h"
#include "mm-errors-types.h"
#include "mm-modem-helpers.h"
#include "mm-iface-modem.h"
#include "mm-iface-modem-3gpp.h"
#include "mm-iface-modem-cdma.h"
#include "mm-sim-qmi.h"

static void iface_modem_init (MMIfaceModem *iface);
static void iface_modem_3gpp_init (MMIfaceModem3gpp *iface);
static void iface_modem_cdma_init (MMIfaceModemCdma *iface);

G_DEFINE_TYPE_EXTENDED (MMBroadbandModemQmi, mm_broadband_modem_qmi, MM_TYPE_BROADBAND_MODEM, 0,
                        G_IMPLEMENT_INTERFACE (MM_TYPE_IFACE_MODEM, iface_modem_init)
                        G_IMPLEMENT_INTERFACE (MM_TYPE_IFACE_MODEM_3GPP, iface_modem_3gpp_init)
                        G_IMPLEMENT_INTERFACE (MM_TYPE_IFACE_MODEM_CDMA, iface_modem_cdma_init));

struct _MMBroadbandModemQmiPrivate {
    /* Cached device IDs, retrieved by the modem interface when loading device
     * IDs, and used afterwards in the 3GPP and CDMA interfaces. */
    gchar *imei;
    gchar *meid;
    gchar *esn;

    /* Allowed mode related */
    gboolean has_mode_preference_in_system_selection_preference;

    /* 3GPP and CDMA share unsolicited events setup/enable/disable/cleanup */
    gboolean unsolicited_events_enabled;
    gboolean unsolicited_events_setup;
    guint event_report_indication_id;
    guint signal_info_indication_id;

    /* 3GPP/CDMA registration helpers */
    gchar *current_operator_id;
    gchar *current_operator_description;
    gboolean unsolicited_registration_events_enabled;
    gboolean unsolicited_registration_events_setup;
    guint serving_system_indication_id;
    guint system_info_indication_id;
};

/*****************************************************************************/

static gboolean
ensure_qmi_client (MMBroadbandModemQmi *self,
                   QmiService service,
                   QmiClient **o_client,
                   GAsyncReadyCallback callback,
                   gpointer user_data)
{
    QmiClient *client;

    client = mm_qmi_port_peek_client (mm_base_modem_peek_port_qmi (MM_BASE_MODEM (self)), service);
    if (!client) {
        g_simple_async_report_error_in_idle (G_OBJECT (self),
                                             callback,
                                             user_data,
                                             MM_CORE_ERROR,
                                             MM_CORE_ERROR_FAILED,
                                             "Couldn't peek client for service '%s'",
                                             qmi_service_get_string (service));
        return FALSE;
    }

    *o_client = client;
    return TRUE;
}

/*****************************************************************************/
/* Capabilities loading (Modem interface) */

static MMModemCapability
modem_load_current_capabilities_finish (MMIfaceModem *self,
                                        GAsyncResult *res,
                                        GError **error)
{
    MMModemCapability caps;
    gchar *caps_str;

    if (g_simple_async_result_propagate_error (G_SIMPLE_ASYNC_RESULT (res), error))
        return MM_MODEM_CAPABILITY_NONE;

    caps = ((MMModemCapability) GPOINTER_TO_UINT (
                g_simple_async_result_get_op_res_gpointer (
                    G_SIMPLE_ASYNC_RESULT (res))));
    caps_str = mm_modem_capability_build_string_from_mask (caps);
    mm_dbg ("loaded current capabilities: %s", caps_str);
    g_free (caps_str);
    return caps;
}

static MMModemCapability
qmi_network_to_modem_capability (QmiDmsRadioInterface network)
{
    switch (network) {
    case QMI_DMS_RADIO_INTERFACE_CDMA20001X:
        return MM_MODEM_CAPABILITY_CDMA_EVDO;
    case QMI_DMS_RADIO_INTERFACE_EVDO:
        return MM_MODEM_CAPABILITY_CDMA_EVDO;
    case QMI_DMS_RADIO_INTERFACE_GSM:
        return MM_MODEM_CAPABILITY_GSM_UMTS;
    case QMI_DMS_RADIO_INTERFACE_UMTS:
        return MM_MODEM_CAPABILITY_GSM_UMTS;
    case QMI_DMS_RADIO_INTERFACE_LTE:
        return MM_MODEM_CAPABILITY_LTE;
    default:
        mm_warn ("Unhandled QMI radio interface received (%u)",
                 (guint)network);
        return MM_MODEM_CAPABILITY_NONE;
    }
}

static void
dms_get_capabilities_ready (QmiClientDms *client,
                            GAsyncResult *res,
                            GSimpleAsyncResult *simple)
{
    QmiMessageDmsGetCapabilitiesOutput *output = NULL;
    GError *error = NULL;

    output = qmi_client_dms_get_capabilities_finish (client, res, &error);
    if (!output) {
        g_prefix_error (&error, "QMI operation failed: ");
        g_simple_async_result_take_error (simple, error);
    } else if (!qmi_message_dms_get_capabilities_output_get_result (output, &error)) {
        g_prefix_error (&error, "Couldn't get Capabilities: ");
        g_simple_async_result_take_error (simple, error);
    } else {
        guint i;
        guint mask = MM_MODEM_CAPABILITY_NONE;
        GArray *radio_interface_list;

        qmi_message_dms_get_capabilities_output_get_info (
            output,
            NULL, /* info_max_tx_channel_rate */
            NULL, /* info_max_rx_channel_rate */
            NULL, /* info_data_service_capability */
            NULL, /* info_sim_capability */
            &radio_interface_list,
            NULL);

        for (i = 0; i < radio_interface_list->len; i++) {
            mask |= qmi_network_to_modem_capability (g_array_index (radio_interface_list,
                                                                    QmiDmsRadioInterface,
                                                                    i));
        }

        g_simple_async_result_set_op_res_gpointer (simple,
                                                   GUINT_TO_POINTER (mask),
                                                   NULL);
    }

    if (output)
        qmi_message_dms_get_capabilities_output_unref (output);

    g_simple_async_result_complete (simple);
    g_object_unref (simple);
}

static void
modem_load_current_capabilities (MMIfaceModem *self,
                                 GAsyncReadyCallback callback,
                                 gpointer user_data)
{
    GSimpleAsyncResult *result;
    QmiClient *client = NULL;

    if (!ensure_qmi_client (MM_BROADBAND_MODEM_QMI (self),
                            QMI_SERVICE_DMS, &client,
                            callback, user_data))
        return;

    result = g_simple_async_result_new (G_OBJECT (self),
                                        callback,
                                        user_data,
                                        modem_load_current_capabilities);

    mm_dbg ("loading current capabilities...");
    qmi_client_dms_get_capabilities (QMI_CLIENT_DMS (client),
                                     NULL,
                                     5,
                                     NULL,
                                     (GAsyncReadyCallback)dms_get_capabilities_ready,
                                     result);
}

/*****************************************************************************/
/* Manufacturer loading (Modem interface) */

static gchar *
modem_load_manufacturer_finish (MMIfaceModem *self,
                                GAsyncResult *res,
                                GError **error)
{
    gchar *manufacturer;

    if (g_simple_async_result_propagate_error (G_SIMPLE_ASYNC_RESULT (res), error))
        return NULL;

    manufacturer = g_strdup (g_simple_async_result_get_op_res_gpointer (G_SIMPLE_ASYNC_RESULT (res)));
    mm_dbg ("loaded manufacturer: %s", manufacturer);
    return manufacturer;
}

static void
dms_get_manufacturer_ready (QmiClientDms *client,
                            GAsyncResult *res,
                            GSimpleAsyncResult *simple)
{
    QmiMessageDmsGetManufacturerOutput *output = NULL;
    GError *error = NULL;

    output = qmi_client_dms_get_manufacturer_finish (client, res, &error);
    if (!output) {
        g_prefix_error (&error, "QMI operation failed: ");
        g_simple_async_result_take_error (simple, error);
    } else if (!qmi_message_dms_get_manufacturer_output_get_result (output, &error)) {
        g_prefix_error (&error, "Couldn't get Manufacturer: ");
        g_simple_async_result_take_error (simple, error);
    } else {
        const gchar *str;

        qmi_message_dms_get_manufacturer_output_get_manufacturer (output, &str, NULL);
        g_simple_async_result_set_op_res_gpointer (simple,
                                                   g_strdup (str),
                                                   (GDestroyNotify)g_free);
    }

    if (output)
        qmi_message_dms_get_manufacturer_output_unref (output);

    g_simple_async_result_complete (simple);
    g_object_unref (simple);
}

static void
modem_load_manufacturer (MMIfaceModem *self,
                         GAsyncReadyCallback callback,
                         gpointer user_data)
{
    GSimpleAsyncResult *result;
    QmiClient *client = NULL;

    if (!ensure_qmi_client (MM_BROADBAND_MODEM_QMI (self),
                            QMI_SERVICE_DMS, &client,
                            callback, user_data))
        return;

    result = g_simple_async_result_new (G_OBJECT (self),
                                        callback,
                                        user_data,
                                        modem_load_manufacturer);

    mm_dbg ("loading manufacturer...");
    qmi_client_dms_get_manufacturer (QMI_CLIENT_DMS (client),
                                     NULL,
                                     5,
                                     NULL,
                                     (GAsyncReadyCallback)dms_get_manufacturer_ready,
                                     result);
}

/*****************************************************************************/
/* Model loading (Modem interface) */

static gchar *
modem_load_model_finish (MMIfaceModem *self,
                         GAsyncResult *res,
                         GError **error)
{
    gchar *model;

    if (g_simple_async_result_propagate_error (G_SIMPLE_ASYNC_RESULT (res), error))
        return NULL;

    model = g_strdup (g_simple_async_result_get_op_res_gpointer (G_SIMPLE_ASYNC_RESULT (res)));
    mm_dbg ("loaded model: %s", model);
    return model;
}

static void
dms_get_model_ready (QmiClientDms *client,
                     GAsyncResult *res,
                     GSimpleAsyncResult *simple)
{
    QmiMessageDmsGetModelOutput *output = NULL;
    GError *error = NULL;

    output = qmi_client_dms_get_model_finish (client, res, &error);
    if (!output) {
        g_prefix_error (&error, "QMI operation failed: ");
        g_simple_async_result_take_error (simple, error);
    } else if (!qmi_message_dms_get_model_output_get_result (output, &error)) {
        g_prefix_error (&error, "Couldn't get Model: ");
        g_simple_async_result_take_error (simple, error);
    } else {
        const gchar *str;

        qmi_message_dms_get_model_output_get_model (output, &str, NULL);
        g_simple_async_result_set_op_res_gpointer (simple,
                                                   g_strdup (str),
                                                   (GDestroyNotify)g_free);
    }

    if (output)
        qmi_message_dms_get_model_output_unref (output);

    g_simple_async_result_complete (simple);
    g_object_unref (simple);
}

static void
modem_load_model (MMIfaceModem *self,
                  GAsyncReadyCallback callback,
                  gpointer user_data)
{
    GSimpleAsyncResult *result;
    QmiClient *client = NULL;

    if (!ensure_qmi_client (MM_BROADBAND_MODEM_QMI (self),
                            QMI_SERVICE_DMS, &client,
                            callback, user_data))
        return;

    result = g_simple_async_result_new (G_OBJECT (self),
                                        callback,
                                        user_data,
                                        modem_load_model);

    mm_dbg ("loading model...");
    qmi_client_dms_get_model (QMI_CLIENT_DMS (client),
                              NULL,
                              5,
                              NULL,
                              (GAsyncReadyCallback)dms_get_model_ready,
                              result);
}

/*****************************************************************************/
/* Revision loading (Modem interface) */

static gchar *
modem_load_revision_finish (MMIfaceModem *self,
                            GAsyncResult *res,
                            GError **error)
{
    gchar *revision;

    if (g_simple_async_result_propagate_error (G_SIMPLE_ASYNC_RESULT (res), error))
        return NULL;

    revision = g_strdup (g_simple_async_result_get_op_res_gpointer (G_SIMPLE_ASYNC_RESULT (res)));
    mm_dbg ("loaded revision: %s", revision);
    return revision;
}

static void
dms_get_revision_ready (QmiClientDms *client,
                        GAsyncResult *res,
                        GSimpleAsyncResult *simple)
{
    QmiMessageDmsGetRevisionOutput *output = NULL;
    GError *error = NULL;

    output = qmi_client_dms_get_revision_finish (client, res, &error);
    if (!output) {
        g_prefix_error (&error, "QMI operation failed: ");
        g_simple_async_result_take_error (simple, error);
    } else if (!qmi_message_dms_get_revision_output_get_result (output, &error)) {
        g_prefix_error (&error, "Couldn't get Revision: ");
        g_simple_async_result_take_error (simple, error);
    } else {
        const gchar *str;

        qmi_message_dms_get_revision_output_get_revision (output, &str, NULL);
        g_simple_async_result_set_op_res_gpointer (simple,
                                                   g_strdup (str),
                                                   (GDestroyNotify)g_free);
    }

    if (output)
        qmi_message_dms_get_revision_output_unref (output);

    g_simple_async_result_complete (simple);
    g_object_unref (simple);
}

static void
modem_load_revision (MMIfaceModem *self,
                     GAsyncReadyCallback callback,
                     gpointer user_data)
{
    GSimpleAsyncResult *result;
    QmiClient *client = NULL;

    if (!ensure_qmi_client (MM_BROADBAND_MODEM_QMI (self),
                            QMI_SERVICE_DMS, &client,
                            callback, user_data))
        return;

    result = g_simple_async_result_new (G_OBJECT (self),
                                        callback,
                                        user_data,
                                        modem_load_revision);

    mm_dbg ("loading revision...");
    qmi_client_dms_get_revision (QMI_CLIENT_DMS (client),
                                 NULL,
                                 5,
                                 NULL,
                                 (GAsyncReadyCallback)dms_get_revision_ready,
                                 result);
}

/*****************************************************************************/
/* Equipment Identifier loading (Modem interface) */

typedef struct {
    MMBroadbandModemQmi *self;
    QmiClient *client;
    GSimpleAsyncResult *result;
} LoadEquipmentIdentifierContext;

static void
load_equipment_identifier_context_complete_and_free (LoadEquipmentIdentifierContext *ctx)
{
    g_simple_async_result_complete (ctx->result);
    g_object_unref (ctx->result);
    g_object_unref (ctx->client);
    g_object_unref (ctx->self);
    g_free (ctx);
}

static gchar *
modem_load_equipment_identifier_finish (MMIfaceModem *self,
                                        GAsyncResult *res,
                                        GError **error)
{
    gchar *equipment_identifier;

    if (g_simple_async_result_propagate_error (G_SIMPLE_ASYNC_RESULT (res), error))
        return NULL;

    equipment_identifier = g_strdup (g_simple_async_result_get_op_res_gpointer (G_SIMPLE_ASYNC_RESULT (res)));
    mm_dbg ("loaded equipment identifier: %s", equipment_identifier);
    return equipment_identifier;
}

static void
dms_get_ids_ready (QmiClientDms *client,
                   GAsyncResult *res,
                   LoadEquipmentIdentifierContext *ctx)
{
    QmiMessageDmsGetIdsOutput *output = NULL;
    GError *error = NULL;
    const gchar *str;

    output = qmi_client_dms_get_ids_finish (client, res, &error);
    if (!output) {
        g_prefix_error (&error, "QMI operation failed: ");
        g_simple_async_result_take_error (ctx->result, error);
        load_equipment_identifier_context_complete_and_free (ctx);
        return;
    }

    if (!qmi_message_dms_get_ids_output_get_result (output, &error)) {
        g_prefix_error (&error, "Couldn't get IDs: ");
        g_simple_async_result_take_error (ctx->result, error);
        qmi_message_dms_get_ids_output_unref (output);
        load_equipment_identifier_context_complete_and_free (ctx);
        return;
    }

    /* In order:
     * If we have a IMEI, use it...
     * Otherwise, if we have a ESN, use it...
     * Otherwise, if we have a MEID, use it...
     * Otherwise, 'unknown'
     */

    if (qmi_message_dms_get_ids_output_get_imei (output, &str, NULL) &&
        str[0] != '\0' && str[0] != '0') {
        g_free (ctx->self->priv->imei);
        ctx->self->priv->imei = g_strdup (str);
    }

    if (qmi_message_dms_get_ids_output_get_esn (output, &str, NULL) &&
        str[0] != '\0' && str[0] != '0') {
        g_free (ctx->self->priv->esn);
        ctx->self->priv->esn = g_strdup (str);
    }

    if (qmi_message_dms_get_ids_output_get_meid (output, &str, NULL) &&
        str[0] != '\0' && str[0] != '0') {
        g_free (ctx->self->priv->meid);
        ctx->self->priv->meid = g_strdup (str);
    }

    if (ctx->self->priv->imei)
        str = ctx->self->priv->imei;
    else if (ctx->self->priv->esn)
        str = ctx->self->priv->esn;
    else if (ctx->self->priv->meid)
        str = ctx->self->priv->meid;
    else
        str = "unknown";

    g_simple_async_result_set_op_res_gpointer (ctx->result,
                                               g_strdup (str),
                                               (GDestroyNotify)g_free);

    qmi_message_dms_get_ids_output_unref (output);
    load_equipment_identifier_context_complete_and_free (ctx);
}

static void
modem_load_equipment_identifier (MMIfaceModem *self,
                                 GAsyncReadyCallback callback,
                                 gpointer user_data)
{
    LoadEquipmentIdentifierContext *ctx;
    QmiClient *client = NULL;

    if (!ensure_qmi_client (MM_BROADBAND_MODEM_QMI (self),
                            QMI_SERVICE_DMS, &client,
                            callback, user_data))
        return;

    ctx = g_new (LoadEquipmentIdentifierContext, 1);
    ctx->self = g_object_ref (self);
    ctx->client = g_object_ref (client);
    ctx->result = g_simple_async_result_new (G_OBJECT (self),
                                             callback,
                                             user_data,
                                             modem_load_equipment_identifier);

    mm_dbg ("loading equipment identifier...");
    qmi_client_dms_get_ids (QMI_CLIENT_DMS (client),
                            NULL,
                            5,
                            NULL,
                            (GAsyncReadyCallback)dms_get_ids_ready,
                            ctx);
}

/*****************************************************************************/
/* Device identifier loading (Modem interface) */

static gchar *
modem_load_device_identifier_finish (MMIfaceModem *self,
                                     GAsyncResult *res,
                                     GError **error)
{
    gchar *device_identifier;

    if (g_simple_async_result_propagate_error (G_SIMPLE_ASYNC_RESULT (res), error))
        return NULL;

    device_identifier = g_strdup (g_simple_async_result_get_op_res_gpointer (G_SIMPLE_ASYNC_RESULT (res)));
    mm_dbg ("loaded device identifier: %s", device_identifier);
    return device_identifier;
}

static void
modem_load_device_identifier (MMIfaceModem *self,
                              GAsyncReadyCallback callback,
                              gpointer user_data)
{
    GSimpleAsyncResult *result;
    gchar *device_identifier;

    mm_dbg ("loading device identifier...");
    result = g_simple_async_result_new (G_OBJECT (self),
                                        callback,
                                        user_data,
                                        modem_load_device_identifier);

    /* Just use dummy ATI/ATI1 replies, all the other internal info should be
     * enough for uniqueness */
    device_identifier = mm_broadband_modem_create_device_identifier (MM_BROADBAND_MODEM (self), "", "");
    g_simple_async_result_set_op_res_gpointer (result,
                                               device_identifier,
                                               (GDestroyNotify)g_free);
    g_simple_async_result_complete_in_idle (result);
    g_object_unref (result);
}

/*****************************************************************************/
/* Own Numbers loading (Modem interface) */

static GStrv
modem_load_own_numbers_finish (MMIfaceModem *self,
                               GAsyncResult *res,
                               GError **error)
{
    gchar **own_numbers;

    if (g_simple_async_result_propagate_error (G_SIMPLE_ASYNC_RESULT (res), error))
        return NULL;

    own_numbers = g_new0 (gchar *, 2);
    own_numbers[0] = g_strdup (g_simple_async_result_get_op_res_gpointer (G_SIMPLE_ASYNC_RESULT (res)));
    mm_dbg ("loaded own numbers: %s", own_numbers[0]);
    return own_numbers;
}

static void
dms_get_msisdn_ready (QmiClientDms *client,
                      GAsyncResult *res,
                      GSimpleAsyncResult *simple)
{
    QmiMessageDmsGetMsisdnOutput *output = NULL;
    GError *error = NULL;

    output = qmi_client_dms_get_msisdn_finish (client, res, &error);
    if (!output) {
        g_prefix_error (&error, "QMI operation failed: ");
        g_simple_async_result_take_error (simple, error);
    } else if (!qmi_message_dms_get_msisdn_output_get_result (output, &error)) {
        g_prefix_error (&error, "Couldn't get MSISDN: ");
        g_simple_async_result_take_error (simple, error);
    } else {
        const gchar *str = NULL;

        qmi_message_dms_get_msisdn_output_get_msisdn (output, &str, NULL);
        g_simple_async_result_set_op_res_gpointer (simple,
                                                   g_strdup (str),
                                                   (GDestroyNotify)g_free);
    }

    if (output)
        qmi_message_dms_get_msisdn_output_unref (output);

    g_simple_async_result_complete (simple);
    g_object_unref (simple);
}

static void
modem_load_own_numbers (MMIfaceModem *self,
                        GAsyncReadyCallback callback,
                        gpointer user_data)
{
    GSimpleAsyncResult *result;
    QmiClient *client = NULL;

    if (!ensure_qmi_client (MM_BROADBAND_MODEM_QMI (self),
                            QMI_SERVICE_DMS, &client,
                            callback, user_data))
        return;

    result = g_simple_async_result_new (G_OBJECT (self),
                                        callback,
                                        user_data,
                                        modem_load_own_numbers);

    mm_dbg ("loading own numbers...");
    qmi_client_dms_get_msisdn (QMI_CLIENT_DMS (client),
                               NULL,
                               5,
                               NULL,
                               (GAsyncReadyCallback)dms_get_msisdn_ready,
                               result);
}

/*****************************************************************************/
/* Check if unlock required (Modem interface) */

static MMModemLock
modem_load_unlock_required_finish (MMIfaceModem *self,
                                   GAsyncResult *res,
                                   GError **error)
{
    if (g_simple_async_result_propagate_error (G_SIMPLE_ASYNC_RESULT (res), error))
        return MM_MODEM_LOCK_UNKNOWN;

    return (MMModemLock) GPOINTER_TO_UINT (g_simple_async_result_get_op_res_gpointer (
                                               G_SIMPLE_ASYNC_RESULT (res)));
}

static MMModemLock
uim_pin_status_to_modem_lock (QmiDmsUimPinStatus status,
                              gboolean pin1) /* TRUE for PIN1, FALSE for PIN2 */
{
    switch (status) {
    case QMI_DMS_UIM_PIN_STATUS_NOT_INITIALIZED:
        return MM_MODEM_LOCK_UNKNOWN;
    case QMI_DMS_UIM_PIN_STATUS_ENABLED_NOT_VERIFIED:
        return pin1 ? MM_MODEM_LOCK_SIM_PIN : MM_MODEM_LOCK_SIM_PIN2;
    case QMI_DMS_UIM_PIN_STATUS_ENABLED_VERIFIED:
        return MM_MODEM_LOCK_NONE;
    case QMI_DMS_UIM_PIN_STATUS_DISABLED:
        return MM_MODEM_LOCK_NONE;
    case QMI_DMS_UIM_PIN_STATUS_BLOCKED:
        return pin1 ? MM_MODEM_LOCK_SIM_PUK : MM_MODEM_LOCK_SIM_PUK2;
    case QMI_DMS_UIM_PIN_STATUS_PERMANENTLY_BLOCKED:
        return MM_MODEM_LOCK_UNKNOWN;
    case QMI_DMS_UIM_PIN_STATUS_UNBLOCKED:
        /* This state is possibly given when after an Unblock() operation has been performed.
         * We'll assume the PIN is verified after this. */
        return MM_MODEM_LOCK_NONE;
    case QMI_DMS_UIM_PIN_STATUS_CHANGED:
        /* This state is possibly given when after an ChangePin() operation has been performed.
         * We'll assume the PIN is verified after this. */
        return MM_MODEM_LOCK_NONE;
    default:
        return MM_MODEM_LOCK_UNKNOWN;
    }
}

static void
dms_uim_get_pin_status_ready (QmiClientDms *client,
                              GAsyncResult *res,
                              GSimpleAsyncResult *simple)
{
    QmiMessageDmsUimGetPinStatusOutput *output;
    GError *error = NULL;

    output = qmi_client_dms_uim_get_pin_status_finish (client, res, &error);
    if (!output) {
        g_prefix_error (&error, "QMI operation failed: ");
        g_simple_async_result_take_error (simple, error);
    } else if (!qmi_message_dms_uim_get_pin_status_output_get_result (output, &error)) {
        /* When no SIM inserted, an internal error when checking PIN status
         * needs to be fatal so that we mark the modem unusable. */
        if (g_error_matches (error,
                             QMI_PROTOCOL_ERROR,
                             QMI_PROTOCOL_ERROR_INTERNAL)) {
            g_simple_async_result_set_error (simple,
                                             MM_MOBILE_EQUIPMENT_ERROR,
                                             MM_MOBILE_EQUIPMENT_ERROR_SIM_FAILURE,
                                             "Couldn't get PIN status: %s",
                                             error->message);
            g_error_free (error);
        } else {
            g_prefix_error (&error, "Couldn't get PIN status: ");
            g_simple_async_result_take_error (simple, error);
        }
    } else {
        MMModemLock lock = MM_MODEM_LOCK_UNKNOWN;
        QmiDmsUimPinStatus current_status;

        if (qmi_message_dms_uim_get_pin_status_output_get_pin1_status (
                output,
                &current_status,
                NULL, /* verify_retries_left */
                NULL, /* unblock_retries_left */
                NULL))
            lock = uim_pin_status_to_modem_lock (current_status, TRUE);

        if (lock == MM_MODEM_LOCK_NONE &&
            qmi_message_dms_uim_get_pin_status_output_get_pin2_status (
                output,
                &current_status,
                NULL, /* verify_retries_left */
                NULL, /* unblock_retries_left */
                NULL))
            lock = uim_pin_status_to_modem_lock (current_status, FALSE);

        g_simple_async_result_set_op_res_gpointer (simple, GUINT_TO_POINTER (lock), NULL);
    }

    if (output)
        qmi_message_dms_uim_get_pin_status_output_unref (output);

    g_simple_async_result_complete (simple);
    g_object_unref (simple);
}

static void
modem_load_unlock_required (MMIfaceModem *self,
                            GAsyncReadyCallback callback,
                            gpointer user_data)
{
    GSimpleAsyncResult *result;
    QmiClient *client = NULL;

    if (!ensure_qmi_client (MM_BROADBAND_MODEM_QMI (self),
                            QMI_SERVICE_DMS, &client,
                            callback, user_data))
        return;

    result = g_simple_async_result_new (G_OBJECT (self),
                                        callback,
                                        user_data,
                                        modem_load_unlock_required);

    mm_dbg ("loading unlock required...");
    qmi_client_dms_uim_get_pin_status (QMI_CLIENT_DMS (client),
                                       NULL,
                                       5,
                                       NULL,
                                       (GAsyncReadyCallback)dms_uim_get_pin_status_ready,
                                       result);
}

/*****************************************************************************/
/* Check if unlock retries (Modem interface) */

static MMUnlockRetries *
modem_load_unlock_retries_finish (MMIfaceModem *self,
                                  GAsyncResult *res,
                                  GError **error)
{
    if (g_simple_async_result_propagate_error (G_SIMPLE_ASYNC_RESULT (res), error))
        return NULL;

    return MM_UNLOCK_RETRIES (g_object_ref (g_simple_async_result_get_op_res_gpointer (G_SIMPLE_ASYNC_RESULT (res))));
}

static void
retry_count_dms_uim_get_pin_status_ready (QmiClientDms *client,
                                          GAsyncResult *res,
                                          GSimpleAsyncResult *simple)
{
    QmiMessageDmsUimGetPinStatusOutput *output;
    GError *error = NULL;

    output = qmi_client_dms_uim_get_pin_status_finish (client, res, &error);
    if (!output) {
        g_prefix_error (&error, "QMI operation failed: ");
        g_simple_async_result_take_error (simple, error);
    } else if (!qmi_message_dms_uim_get_pin_status_output_get_result (output, &error)) {
        g_prefix_error (&error, "Couldn't get unlock retries: ");
        g_simple_async_result_take_error (simple, error);
    } else {
        MMUnlockRetries *retries;
        guint8 verify_retries_left;
        guint8 unblock_retries_left;

        retries = mm_unlock_retries_new ();

        if (qmi_message_dms_uim_get_pin_status_output_get_pin1_status (
                output,
                NULL, /* current_status */
                &verify_retries_left,
                &unblock_retries_left,
                NULL)) {
            mm_unlock_retries_set (retries, MM_MODEM_LOCK_SIM_PIN, verify_retries_left);
            mm_unlock_retries_set (retries, MM_MODEM_LOCK_SIM_PUK, unblock_retries_left);
        }

        if (qmi_message_dms_uim_get_pin_status_output_get_pin2_status (
                output,
                NULL, /* current_status */
                &verify_retries_left,
                &unblock_retries_left,
                NULL)) {
            mm_unlock_retries_set (retries, MM_MODEM_LOCK_SIM_PIN2, verify_retries_left);
            mm_unlock_retries_set (retries, MM_MODEM_LOCK_SIM_PUK2, unblock_retries_left);
        }

        g_simple_async_result_set_op_res_gpointer (simple, retries, g_object_unref);
    }

    if (output)
        qmi_message_dms_uim_get_pin_status_output_unref (output);

    g_simple_async_result_complete (simple);
    g_object_unref (simple);
}

static void
modem_load_unlock_retries (MMIfaceModem *self,
                           GAsyncReadyCallback callback,
                           gpointer user_data)
{
    GSimpleAsyncResult *result;
    QmiClient *client = NULL;

    if (!ensure_qmi_client (MM_BROADBAND_MODEM_QMI (self),
                            QMI_SERVICE_DMS, &client,
                            callback, user_data))
        return;

    result = g_simple_async_result_new (G_OBJECT (self),
                                        callback,
                                        user_data,
                                        modem_load_unlock_retries);

    mm_dbg ("loading unlock retries...");
    qmi_client_dms_uim_get_pin_status (QMI_CLIENT_DMS (client),
                                       NULL,
                                       5,
                                       NULL,
                                       (GAsyncReadyCallback)retry_count_dms_uim_get_pin_status_ready,
                                       result);
}

/*****************************************************************************/
/* Load supported bands (Modem interface) */

static GArray *
modem_load_supported_bands_finish (MMIfaceModem *self,
                                   GAsyncResult *res,
                                   GError **error)
{
    if (g_simple_async_result_propagate_error (G_SIMPLE_ASYNC_RESULT (res), error))
        return NULL;

    return (GArray *) g_array_ref (g_simple_async_result_get_op_res_gpointer (
                                       G_SIMPLE_ASYNC_RESULT (res)));
}

typedef struct {
    QmiDmsBandCapability qmi_band;
    MMModemBand mm_band;
} BandsMap;

static const BandsMap bands_map [] = {
    /* CDMA bands */
    {
        (QMI_DMS_BAND_CAPABILITY_BC_0_A_SYSTEM | QMI_DMS_BAND_CAPABILITY_BC_0_B_SYSTEM),
        MM_MODEM_BAND_CDMA_BC0_CELLULAR_800
    },
    { QMI_DMS_BAND_CAPABILITY_BC_1_ALL_BLOCKS, MM_MODEM_BAND_CDMA_BC1_PCS_1900       },
    { QMI_DMS_BAND_CAPABILITY_BC_2,            MM_MODEM_BAND_CDMA_BC2_TACS           },
    { QMI_DMS_BAND_CAPABILITY_BC_3_A_SYSTEM,   MM_MODEM_BAND_CDMA_BC3_JTACS          },
    { QMI_DMS_BAND_CAPABILITY_BC_4_ALL_BLOCKS, MM_MODEM_BAND_CDMA_BC4_KOREAN_PCS     },
    { QMI_DMS_BAND_CAPABILITY_BC_5_ALL_BLOCKS, MM_MODEM_BAND_CDMA_BC5_NMT450         },
    { QMI_DMS_BAND_CAPABILITY_BC_6,            MM_MODEM_BAND_CDMA_BC6_IMT2000        },
    { QMI_DMS_BAND_CAPABILITY_BC_7,            MM_MODEM_BAND_CDMA_BC7_CELLULAR_700   },
    { QMI_DMS_BAND_CAPABILITY_BC_8,            MM_MODEM_BAND_CDMA_BC8_1800           },
    { QMI_DMS_BAND_CAPABILITY_BC_9,            MM_MODEM_BAND_CDMA_BC9_900            },
    { QMI_DMS_BAND_CAPABILITY_BC_10,           MM_MODEM_BAND_CDMA_BC10_SECONDARY_800 },
    { QMI_DMS_BAND_CAPABILITY_BC_11,           MM_MODEM_BAND_CDMA_BC11_PAMR_400      },
    { QMI_DMS_BAND_CAPABILITY_BC_12,           MM_MODEM_BAND_CDMA_BC12_PAMR_800      },
    { QMI_DMS_BAND_CAPABILITY_BC_14,           MM_MODEM_BAND_CDMA_BC14_PCS2_1900     },
    { QMI_DMS_BAND_CAPABILITY_BC_15,           MM_MODEM_BAND_CDMA_BC15_AWS           },
    { QMI_DMS_BAND_CAPABILITY_BC_16,           MM_MODEM_BAND_CDMA_BC16_US_2500       },
    { QMI_DMS_BAND_CAPABILITY_BC_17,           MM_MODEM_BAND_CDMA_BC17_US_FLO_2500   },
    { QMI_DMS_BAND_CAPABILITY_BC_18,           MM_MODEM_BAND_CDMA_BC18_US_PS_700     },
    { QMI_DMS_BAND_CAPABILITY_BC_19,           MM_MODEM_BAND_CDMA_BC19_US_LOWER_700  },

    /* GSM bands */
    { QMI_DMS_BAND_CAPABILITY_GSM_DCS_1800,     MM_MODEM_BAND_DCS  },
    { QMI_DMS_BAND_CAPABILITY_GSM_900_EXTENDED, MM_MODEM_BAND_EGSM },
    { QMI_DMS_BAND_CAPABILITY_GSM_PCS_1900,     MM_MODEM_BAND_PCS  },
    { QMI_DMS_BAND_CAPABILITY_GSM_850,          MM_MODEM_BAND_G850 },

    /* UMTS/WCDMA bands */
    { QMI_DMS_BAND_CAPABILITY_WCDMA_2100,       MM_MODEM_BAND_U2100 },
    { QMI_DMS_BAND_CAPABILITY_WCDMA_DCS_1800,   MM_MODEM_BAND_U1800 },
    { QMI_DMS_BAND_CAPABILITY_WCDMA_1700_US,    MM_MODEM_BAND_U17IV },
    { QMI_DMS_BAND_CAPABILITY_WCDMA_800,        MM_MODEM_BAND_U800  },
    {
        (QMI_DMS_BAND_CAPABILITY_WCDMA_850_US | QMI_DMS_BAND_CAPABILITY_WCDMA_850_JAPAN),
        MM_MODEM_BAND_U850
    },
    { QMI_DMS_BAND_CAPABILITY_WCDMA_900,        MM_MODEM_BAND_U900  },
    { QMI_DMS_BAND_CAPABILITY_WCDMA_1700_JAPAN, MM_MODEM_BAND_U17IX },
    { QMI_DMS_BAND_CAPABILITY_WCDMA_2600,       MM_MODEM_BAND_U2600 }

    /* NOTE. The following bands were unmatched:
     *
     * - QMI_DMS_BAND_CAPABILITY_GSM_900_PRIMARY
     * - QMI_DMS_BAND_CAPABILITY_GSM_450
     * - QMI_DMS_BAND_CAPABILITY_GSM_480
     * - QMI_DMS_BAND_CAPABILITY_GSM_750
     * - QMI_DMS_BAND_CAPABILITY_GSM_900_RAILWAILS
     * - QMI_DMS_BAND_CAPABILITY_WCDMA_1500
     * - MM_MODEM_BAND_CDMA_BC13_IMT2000_2500
     * - MM_MODEM_BAND_U1900
     */
};

static void
add_qmi_bands (GArray *mm_bands,
               QmiDmsBandCapability qmi_bands)
{
    static QmiDmsBandCapability qmi_bands_expected = 0;
    QmiDmsBandCapability not_expected;
    guint i;

    g_assert (mm_bands != NULL);

    /* Build mask of expected bands only once */
    if (G_UNLIKELY (qmi_bands_expected == 0)) {
        for (i = 0; i < G_N_ELEMENTS (bands_map); i++) {
            qmi_bands_expected |= bands_map[i].qmi_band;
        }
    }

    /* Log about the bands that cannot be represented in ModemManager */
    not_expected = ((qmi_bands_expected ^ qmi_bands) & qmi_bands);
    if (not_expected) {
        gchar *aux;

        aux = qmi_dms_band_capability_build_string_from_mask (not_expected);
        mm_dbg ("Cannot add the following bands: '%s'", aux);
        g_free (aux);
    }

    /* And add the expected ones */
    for (i = 0; i < G_N_ELEMENTS (bands_map); i++) {
        if (qmi_bands & bands_map[i].qmi_band)
            g_array_append_val (mm_bands, bands_map[i].mm_band);
    }
}

typedef struct {
    QmiDmsLteBandCapability qmi_band;
    MMModemBand mm_band;
} LteBandsMap;

static const LteBandsMap lte_bands_map [] = {
    { QMI_DMS_LTE_BAND_CAPABILITY_EUTRAN_1,  MM_MODEM_BAND_EUTRAN_I       },
    { QMI_DMS_LTE_BAND_CAPABILITY_EUTRAN_2,  MM_MODEM_BAND_EUTRAN_II      },
    { QMI_DMS_LTE_BAND_CAPABILITY_EUTRAN_3,  MM_MODEM_BAND_EUTRAN_III     },
    { QMI_DMS_LTE_BAND_CAPABILITY_EUTRAN_4,  MM_MODEM_BAND_EUTRAN_IV      },
    { QMI_DMS_LTE_BAND_CAPABILITY_EUTRAN_5,  MM_MODEM_BAND_EUTRAN_V       },
    { QMI_DMS_LTE_BAND_CAPABILITY_EUTRAN_6,  MM_MODEM_BAND_EUTRAN_VI      },
    { QMI_DMS_LTE_BAND_CAPABILITY_EUTRAN_7,  MM_MODEM_BAND_EUTRAN_VII     },
    { QMI_DMS_LTE_BAND_CAPABILITY_EUTRAN_8,  MM_MODEM_BAND_EUTRAN_VIII    },
    { QMI_DMS_LTE_BAND_CAPABILITY_EUTRAN_9,  MM_MODEM_BAND_EUTRAN_IX      },
    { QMI_DMS_LTE_BAND_CAPABILITY_EUTRAN_10, MM_MODEM_BAND_EUTRAN_X       },
    { QMI_DMS_LTE_BAND_CAPABILITY_EUTRAN_11, MM_MODEM_BAND_EUTRAN_XI      },
    { QMI_DMS_LTE_BAND_CAPABILITY_EUTRAN_12, MM_MODEM_BAND_EUTRAN_XII     },
    { QMI_DMS_LTE_BAND_CAPABILITY_EUTRAN_13, MM_MODEM_BAND_EUTRAN_XIII    },
    { QMI_DMS_LTE_BAND_CAPABILITY_EUTRAN_14, MM_MODEM_BAND_EUTRAN_XIV     },
    { QMI_DMS_LTE_BAND_CAPABILITY_EUTRAN_17, MM_MODEM_BAND_EUTRAN_XVII    },
    { QMI_DMS_LTE_BAND_CAPABILITY_EUTRAN_18, MM_MODEM_BAND_EUTRAN_XVIII   },
    { QMI_DMS_LTE_BAND_CAPABILITY_EUTRAN_19, MM_MODEM_BAND_EUTRAN_XIX     },
    { QMI_DMS_LTE_BAND_CAPABILITY_EUTRAN_20, MM_MODEM_BAND_EUTRAN_XX      },
    { QMI_DMS_LTE_BAND_CAPABILITY_EUTRAN_21, MM_MODEM_BAND_EUTRAN_XXI     },
    { QMI_DMS_LTE_BAND_CAPABILITY_EUTRAN_24, MM_MODEM_BAND_EUTRAN_XXIV    },
    { QMI_DMS_LTE_BAND_CAPABILITY_EUTRAN_25, MM_MODEM_BAND_EUTRAN_XXV     },
    { QMI_DMS_LTE_BAND_CAPABILITY_EUTRAN_33, MM_MODEM_BAND_EUTRAN_XXXIII  },
    { QMI_DMS_LTE_BAND_CAPABILITY_EUTRAN_34, MM_MODEM_BAND_EUTRAN_XXXIV   },
    { QMI_DMS_LTE_BAND_CAPABILITY_EUTRAN_35, MM_MODEM_BAND_EUTRAN_XXXV    },
    { QMI_DMS_LTE_BAND_CAPABILITY_EUTRAN_36, MM_MODEM_BAND_EUTRAN_XXXVI   },
    { QMI_DMS_LTE_BAND_CAPABILITY_EUTRAN_37, MM_MODEM_BAND_EUTRAN_XXXVII  },
    { QMI_DMS_LTE_BAND_CAPABILITY_EUTRAN_38, MM_MODEM_BAND_EUTRAN_XXXVIII },
    { QMI_DMS_LTE_BAND_CAPABILITY_EUTRAN_39, MM_MODEM_BAND_EUTRAN_XXXIX   },
    { QMI_DMS_LTE_BAND_CAPABILITY_EUTRAN_40, MM_MODEM_BAND_EUTRAN_XL      },
    { QMI_DMS_LTE_BAND_CAPABILITY_EUTRAN_41, MM_MODEM_BAND_EUTRAN_XLI     },
    { QMI_DMS_LTE_BAND_CAPABILITY_EUTRAN_42, MM_MODEM_BAND_EUTRAN_XLI     },
    { QMI_DMS_LTE_BAND_CAPABILITY_EUTRAN_43, MM_MODEM_BAND_EUTRAN_XLIII   }

    /* NOTE. The following bands were unmatched:
     *
     * - MM_MODEM_BAND_EUTRAN_XXII
     * - MM_MODEM_BAND_EUTRAN_XXIII
     * - MM_MODEM_BAND_EUTRAN_XXVI
     */
};

static void
add_qmi_lte_bands (GArray *mm_bands,
                   QmiDmsLteBandCapability qmi_bands)
{
    /* All QMI LTE bands have a counterpart in ModemManager, no need to check
     * for unexpected ones */
    guint i;

    g_assert (mm_bands != NULL);

    for (i = 0; i < G_N_ELEMENTS (lte_bands_map); i++) {
        if (qmi_bands & lte_bands_map[i].qmi_band)
            g_array_append_val (mm_bands, lte_bands_map[i].mm_band);
    }
}

static void
dms_get_band_capabilities_ready (QmiClientDms *client,
                                 GAsyncResult *res,
                                 GSimpleAsyncResult *simple)
{
    QmiMessageDmsGetBandCapabilitiesOutput *output;
    GError *error = NULL;

    output = qmi_client_dms_get_band_capabilities_finish (client, res, &error);
    if (!output) {
        g_prefix_error (&error, "QMI operation failed: ");
        g_simple_async_result_take_error (simple, error);
    } else if (!qmi_message_dms_get_band_capabilities_output_get_result (output, &error)) {
        g_prefix_error (&error, "Couldn't get band capabilities: ");
        g_simple_async_result_take_error (simple, error);
    } else {
        GArray *mm_bands;
        QmiDmsBandCapability qmi_bands;
        QmiDmsLteBandCapability qmi_lte_bands;

        mm_bands = g_array_new (FALSE, FALSE, sizeof (MMModemBand));

        qmi_message_dms_get_band_capabilities_output_get_band_capability (
            output,
            &qmi_bands,
            NULL);

        add_qmi_bands (mm_bands, qmi_bands);

        if (qmi_message_dms_get_band_capabilities_output_get_lte_band_capability (
                output,
                &qmi_lte_bands,
                NULL)) {
            add_qmi_lte_bands (mm_bands, qmi_lte_bands);
        }

        if (mm_bands->len == 0) {
            g_array_unref (mm_bands);
            g_simple_async_result_set_error (simple,
                                             MM_CORE_ERROR,
                                             MM_CORE_ERROR_FAILED,
                                             "Couldn't parse the list of supported bands");
        } else {
            g_simple_async_result_set_op_res_gpointer (simple,
                                                       mm_bands,
                                                       (GDestroyNotify)g_array_unref);
        }
    }

    if (output)
        qmi_message_dms_get_band_capabilities_output_unref (output);

    g_simple_async_result_complete (simple);
    g_object_unref (simple);
}

static void
modem_load_supported_bands (MMIfaceModem *self,
                            GAsyncReadyCallback callback,
                            gpointer user_data)
{
    GSimpleAsyncResult *result;
    QmiClient *client = NULL;

    if (!ensure_qmi_client (MM_BROADBAND_MODEM_QMI (self),
                            QMI_SERVICE_DMS, &client,
                            callback, user_data))
        return;

    result = g_simple_async_result_new (G_OBJECT (self),
                                        callback,
                                        user_data,
                                        modem_load_supported_bands);

    mm_dbg ("loading band capabilities...");
    qmi_client_dms_get_band_capabilities (QMI_CLIENT_DMS (client),
                                          NULL,
                                          5,
                                          NULL,
                                          (GAsyncReadyCallback)dms_get_band_capabilities_ready,
                                          result);
}

/*****************************************************************************/
/* Load signal quality (Modem interface) */

/* Limit the value betweeen [-113,-51] and scale it to a percentage */
#define STRENGTH_TO_QUALITY(strength)                                   \
    (guint8)(100 - ((CLAMP (strength, -113, -51) + 51) * 100 / (-113 + 51)))

typedef struct {
    MMBroadbandModemQmi *self;
    QmiClient *client;
    GSimpleAsyncResult *result;
} LoadSignalQualityContext;

static void
load_signal_quality_context_complete_and_free (LoadSignalQualityContext *ctx)
{
    g_simple_async_result_complete (ctx->result);
    g_object_unref (ctx->result);
    g_object_unref (ctx->client);
    g_object_unref (ctx->self);
    g_free (ctx);
}

static guint
load_signal_quality_finish (MMIfaceModem *self,
                            GAsyncResult *res,
                            GError **error)
{
    if (g_simple_async_result_propagate_error (G_SIMPLE_ASYNC_RESULT (res), error))
        return 0;

    return GPOINTER_TO_UINT (g_simple_async_result_get_op_res_gpointer (
                                 G_SIMPLE_ASYNC_RESULT (res)));
}

static gint8
signal_info_get_quality (MMBroadbandModemQmi *self,
                         QmiMessageNasGetSignalInfoOutput *output)
{
    gint8 rssi_max = 0;
    gint8 rssi;
    guint8 quality;

    /* We do not report per-technology signal quality, so just get the highest
     * one of the ones reported. */

    if (qmi_message_nas_get_signal_info_output_get_cdma_signal_strength (output, &rssi, NULL, NULL)) {
        mm_dbg ("RSSI (CDMA): %d dBm", rssi);
        rssi = MAX (rssi, rssi_max);
    }

    if (qmi_message_nas_get_signal_info_output_get_hdr_signal_strength (output, &rssi, NULL, NULL, NULL, NULL)) {
        mm_dbg ("RSSI (HDR): %d dBm", rssi);
        rssi = MAX (rssi, rssi_max);
    }

    if (qmi_message_nas_get_signal_info_output_get_gsm_signal_strength (output, &rssi, NULL)) {
        mm_dbg ("RSSI (GSM): %d dBm", rssi);
        rssi = MAX (rssi, rssi_max);
    }

    if (qmi_message_nas_get_signal_info_output_get_wcdma_signal_strength (output, &rssi, NULL, NULL)) {
        mm_dbg ("RSSI (WCDMA): %d dBm", rssi);
        rssi = MAX (rssi, rssi_max);
    }

    if (qmi_message_nas_get_signal_info_output_get_lte_signal_strength (output, &rssi, NULL, NULL, NULL, NULL)) {
        mm_dbg ("RSSI (LTE): %d dBm", rssi);
        rssi = MAX (rssi, rssi_max);
    }

    /* This RSSI comes as negative dBms */
    quality = STRENGTH_TO_QUALITY (rssi_max);

    mm_dbg ("RSSI: %d dBm --> %u%%", rssi_max, quality);
    return quality;
}

static void
get_signal_info_ready (QmiClientNas *client,
                       GAsyncResult *res,
                       LoadSignalQualityContext *ctx)
{
    QmiMessageNasGetSignalInfoOutput *output;
    GError *error = NULL;
    guint quality;

    output = qmi_client_nas_get_signal_info_finish (client, res, &error);
    if (!output) {
        g_simple_async_result_take_error (ctx->result, error);
        load_signal_quality_context_complete_and_free (ctx);
        return;
    }

    if (!qmi_message_nas_get_signal_info_output_get_result (output, &error)) {
        qmi_message_nas_get_signal_info_output_unref (output);
        g_simple_async_result_take_error (ctx->result, error);
        load_signal_quality_context_complete_and_free (ctx);
        return;
    }

    quality = signal_info_get_quality (ctx->self, output);

    g_simple_async_result_set_op_res_gpointer (
        ctx->result,
        GUINT_TO_POINTER (quality),
        NULL);

    qmi_message_nas_get_signal_info_output_unref (output);
    load_signal_quality_context_complete_and_free (ctx);
}

static gint8
signal_strength_get_quality (MMBroadbandModemQmi *self,
                             QmiMessageNasGetSignalStrengthOutput *output)
{
    GArray *array = NULL;
    gint8 signal_max;
    QmiNasRadioInterface main_interface;
    guint8 quality;

    /* We do not report per-technology signal quality, so just get the highest
     * one of the ones reported. */

    /* The mandatory one is always present */
    qmi_message_nas_get_signal_strength_output_get_signal_strength (output, &signal_max, &main_interface, NULL);
    mm_dbg ("Signal strength (%s): %d dBm",
            qmi_nas_radio_interface_get_string (main_interface),
            signal_max);

    /* On multimode devices we may get more */
    if (qmi_message_nas_get_signal_strength_output_get_strength_list (output, &array, NULL)) {
        guint i;

        for (i = 0; i < array->len; i++) {
            QmiMessageNasGetSignalStrengthOutputStrengthListElement *element;

            element = &g_array_index (array, QmiMessageNasGetSignalStrengthOutputStrengthListElement, i);

            mm_dbg ("Signal strength (%s): %d dBm",
                    qmi_nas_radio_interface_get_string (element->radio_interface),
                    element->strength);

            signal_max = MAX (element->strength, signal_max);
        }
    }

    /* This signal strength comes as negative dBms */
    quality = STRENGTH_TO_QUALITY (signal_max);

    mm_dbg ("Signal strength: %d dBm --> %u%%", signal_max, quality);
    return quality;
}

static void
get_signal_strength_ready (QmiClientNas *client,
                           GAsyncResult *res,
                           LoadSignalQualityContext *ctx)
{
    QmiMessageNasGetSignalStrengthOutput *output;
    GError *error = NULL;
    guint quality;

    output = qmi_client_nas_get_signal_strength_finish (client, res, &error);
    if (!output) {
        g_simple_async_result_take_error (ctx->result, error);
        load_signal_quality_context_complete_and_free (ctx);
        return;
    }

    if (!qmi_message_nas_get_signal_strength_output_get_result (output, &error)) {
        qmi_message_nas_get_signal_strength_output_unref (output);
        g_simple_async_result_take_error (ctx->result, error);
        load_signal_quality_context_complete_and_free (ctx);
        return;
    }

    quality = signal_strength_get_quality (ctx->self, output);

    g_simple_async_result_set_op_res_gpointer (
        ctx->result,
        GUINT_TO_POINTER (quality),
        NULL);

    qmi_message_nas_get_signal_strength_output_unref (output);
    load_signal_quality_context_complete_and_free (ctx);
}

static void
load_signal_quality (MMIfaceModem *self,
                     GAsyncReadyCallback callback,
                     gpointer user_data)
{
    LoadSignalQualityContext *ctx;
    QmiClient *client = NULL;

    if (!ensure_qmi_client (MM_BROADBAND_MODEM_QMI (self),
                            QMI_SERVICE_NAS, &client,
                            callback, user_data))
        return;

    ctx = g_new0 (LoadSignalQualityContext, 1);
    ctx->self = g_object_ref (self);
    ctx->client = g_object_ref (client);
    ctx->result = g_simple_async_result_new (G_OBJECT (self),
                                             callback,
                                             user_data,
                                             load_signal_quality);

    mm_dbg ("loading signal quality...");

    /* Signal info introduced in NAS 1.8 */
    if (qmi_client_check_version (ctx->client, 1, 8))
        qmi_client_nas_get_signal_info (QMI_CLIENT_NAS (ctx->client),
                                        NULL,
                                        10,
                                        NULL,
                                        (GAsyncReadyCallback)get_signal_info_ready,
                                        ctx);
    else
        qmi_client_nas_get_signal_strength (QMI_CLIENT_NAS (ctx->client),
                                            NULL,
                                            10,
                                            NULL,
                                            (GAsyncReadyCallback)get_signal_strength_ready,
                                            ctx);
}

/*****************************************************************************/
/* Powering up the modem (Modem interface) */

static gboolean
modem_power_up_down_finish (MMIfaceModem *self,
                            GAsyncResult *res,
                            GError **error)
{
    return !g_simple_async_result_propagate_error (G_SIMPLE_ASYNC_RESULT (res), error);
}

static void
dms_set_operating_mode_ready (QmiClientDms *client,
                              GAsyncResult *res,
                              GSimpleAsyncResult *simple)
{
    QmiMessageDmsSetOperatingModeOutput *output = NULL;
    GError *error = NULL;

    output = qmi_client_dms_set_operating_mode_finish (client, res, &error);
    if (!output) {
        if (g_error_matches (error,
                             QMI_CORE_ERROR,
                             QMI_CORE_ERROR_UNSUPPORTED)) {
            mm_dbg ("Device doesn't support operating mode setting. Ignoring power up/down");
            g_simple_async_result_set_op_res_gboolean (simple, TRUE);
            g_error_free (error);
        } else {
            g_prefix_error (&error, "QMI operation failed: ");
            g_simple_async_result_take_error (simple, error);
        }
    } else if (!qmi_message_dms_set_operating_mode_output_get_result (output, &error)) {
        g_prefix_error (&error, "Couldn't set operating mode: ");
        g_simple_async_result_take_error (simple, error);
    } else {
        g_simple_async_result_set_op_res_gboolean (simple, TRUE);
    }

    if (output)
        qmi_message_dms_set_operating_mode_output_unref (output);

    g_simple_async_result_complete (simple);
    g_object_unref (simple);
}

static void
common_power_up_down (MMIfaceModem *self,
                      QmiDmsOperatingMode mode,
                      GAsyncReadyCallback callback,
                      gpointer user_data)
{
    QmiMessageDmsSetOperatingModeInput *input;
    GSimpleAsyncResult *result;
    QmiClient *client = NULL;
    GError *error = NULL;

    if (!ensure_qmi_client (MM_BROADBAND_MODEM_QMI (self),
                            QMI_SERVICE_DMS, &client,
                            callback, user_data))
        return;

    result = g_simple_async_result_new (G_OBJECT (self),
                                        callback,
                                        user_data,
                                        common_power_up_down);

    input = qmi_message_dms_set_operating_mode_input_new ();
    if (!qmi_message_dms_set_operating_mode_input_set_mode (
            input,
            mode,
            &error)) {
        qmi_message_dms_set_operating_mode_input_unref (input);
        g_simple_async_result_take_error (result, error);
        g_simple_async_result_complete_in_idle (result);
        g_object_unref (result);
        return;
    }

    mm_dbg ("Setting device operating mode...");
    qmi_client_dms_set_operating_mode (QMI_CLIENT_DMS (client),
                                       input,
                                       20,
                                       NULL,
                                       (GAsyncReadyCallback)dms_set_operating_mode_ready,
                                       result);
}

static void
modem_power_down (MMIfaceModem *self,
                  GAsyncReadyCallback callback,
                  gpointer user_data)
{
    common_power_up_down (self,
                          QMI_DMS_OPERATING_MODE_LOW_POWER,
                          callback,
                          user_data);
}

static void
modem_power_up (MMIfaceModem *self,
                GAsyncReadyCallback callback,
                gpointer user_data)
{
    common_power_up_down (self,
                          QMI_DMS_OPERATING_MODE_ONLINE,
                          callback,
                          user_data);
}

/*****************************************************************************/
/* Create SIM (Modem interface) */

static MMSim *
create_sim_finish (MMIfaceModem *self,
                   GAsyncResult *res,
                   GError **error)
{
    return mm_sim_qmi_new_finish (res, error);
}

static void
create_sim (MMIfaceModem *self,
            GAsyncReadyCallback callback,
            gpointer user_data)
{
    /* New QMI SIM */
    mm_sim_qmi_new (MM_BASE_MODEM (self),
                    NULL, /* cancellable */
                    callback,
                    user_data);
}

/*****************************************************************************/
/* Factory reset (Modem interface) */

static gboolean
modem_factory_reset_finish (MMIfaceModem *self,
                            GAsyncResult *res,
                            GError **error)
{
    return !g_simple_async_result_propagate_error (G_SIMPLE_ASYNC_RESULT (res), error);
}

static void
dms_restore_factory_defaults_ready (QmiClientDms *client,
                                    GAsyncResult *res,
                                    GSimpleAsyncResult *simple)
{
    QmiMessageDmsRestoreFactoryDefaultsOutput *output = NULL;
    GError *error = NULL;

    output = qmi_client_dms_restore_factory_defaults_finish (client, res, &error);
    if (!output) {
        g_prefix_error (&error, "QMI operation failed: ");
        g_simple_async_result_take_error (simple, error);
    } else if (!qmi_message_dms_restore_factory_defaults_output_get_result (output, &error)) {
        g_prefix_error (&error, "Couldn't restore factory defaults: ");
        g_simple_async_result_take_error (simple, error);
    } else {
        g_simple_async_result_set_op_res_gboolean (simple, TRUE);
    }

    if (output)
        qmi_message_dms_restore_factory_defaults_output_unref (output);

    g_simple_async_result_complete (simple);
    g_object_unref (simple);
}

static void
modem_factory_reset (MMIfaceModem *self,
                     const gchar *code,
                     GAsyncReadyCallback callback,
                     gpointer user_data)
{
    QmiMessageDmsRestoreFactoryDefaultsInput *input;
    GSimpleAsyncResult *result;
    QmiClient *client = NULL;
    GError *error = NULL;

    if (!ensure_qmi_client (MM_BROADBAND_MODEM_QMI (self),
                            QMI_SERVICE_DMS, &client,
                            callback, user_data))
        return;

    result = g_simple_async_result_new (G_OBJECT (self),
                                        callback,
                                        user_data,
                                        modem_factory_reset);

    input = qmi_message_dms_restore_factory_defaults_input_new ();
    if (!qmi_message_dms_restore_factory_defaults_input_set_service_programming_code (
            input,
            code,
            &error)) {
        qmi_message_dms_restore_factory_defaults_input_unref (input);
        g_simple_async_result_take_error (result, error);
        g_simple_async_result_complete_in_idle (result);
        g_object_unref (result);
        return;
    }

    mm_dbg ("performing a factory reset...");
    qmi_client_dms_restore_factory_defaults (QMI_CLIENT_DMS (client),
                                             input,
                                             10,
                                             NULL,
                                             (GAsyncReadyCallback)dms_restore_factory_defaults_ready,
                                             result);
}

/*****************************************************************************/
/* Load allowed modes (Modem interface) */

typedef struct {
    MMBroadbandModemQmi *self;
    QmiClientNas *client;
    GSimpleAsyncResult *result;
    gboolean run_get_system_selection_preference;
    gboolean run_get_technology_preference;
} LoadAllowedModesContext;

typedef struct {
    MMModemMode allowed;
    MMModemMode preferred;
} LoadAllowedModesResult;

static void
load_allowed_modes_context_complete_and_free (LoadAllowedModesContext *ctx)
{
    g_simple_async_result_complete_in_idle (ctx->result);
    g_object_unref (ctx->result);
    g_object_unref (ctx->client);
    g_object_unref (ctx->self);
    g_free (ctx);
}

static gboolean
load_allowed_modes_finish (MMIfaceModem *self,
                           GAsyncResult *res,
                           MMModemMode *allowed,
                           MMModemMode *preferred,
                           GError **error)
{
    LoadAllowedModesResult *result;

    if (g_simple_async_result_propagate_error (G_SIMPLE_ASYNC_RESULT (res), error))
        return FALSE;

    result = g_simple_async_result_get_op_res_gpointer (G_SIMPLE_ASYNC_RESULT (res));
    *allowed = result->allowed;
    *preferred = result->preferred;
    return TRUE;
}

static void load_allowed_modes_context_step (LoadAllowedModesContext *ctx);

static MMModemMode
modem_mode_from_qmi_radio_technology_preference (QmiNasRatModePreference qmi)
{
    MMModemMode mode = MM_MODEM_MODE_NONE;

    if (qmi & QMI_NAS_RADIO_TECHNOLOGY_PREFERENCE_3GPP2) {
        if (qmi & QMI_NAS_RADIO_TECHNOLOGY_PREFERENCE_AMPS_OR_GSM)
            mode |= MM_MODEM_MODE_CS; /* AMPS */
        if (qmi & QMI_NAS_RADIO_TECHNOLOGY_PREFERENCE_CDMA_OR_WCDMA)
            mode |= MM_MODEM_MODE_2G; /* CDMA */
        if (qmi & QMI_NAS_RADIO_TECHNOLOGY_PREFERENCE_HDR)
            mode |= MM_MODEM_MODE_3G; /* EV-DO */
    }

    if (qmi & QMI_NAS_RADIO_TECHNOLOGY_PREFERENCE_3GPP) {
        if (qmi & QMI_NAS_RADIO_TECHNOLOGY_PREFERENCE_AMPS_OR_GSM)
            mode |= (MM_MODEM_MODE_CS | MM_MODEM_MODE_2G); /* GSM */
        if (qmi & QMI_NAS_RADIO_TECHNOLOGY_PREFERENCE_CDMA_OR_WCDMA)
            mode |= MM_MODEM_MODE_3G; /* WCDMA */
    }

    if (qmi & QMI_NAS_RADIO_TECHNOLOGY_PREFERENCE_LTE)
        mode |= MM_MODEM_MODE_4G;

    return mode;
}

static void
get_technology_preference_ready (QmiClientNas *client,
                                 GAsyncResult *res,
                                 LoadAllowedModesContext *ctx)
{
    LoadAllowedModesResult *result = NULL;
    QmiMessageNasGetTechnologyPreferenceOutput *output = NULL;
    GError *error = NULL;

    output = qmi_client_nas_get_technology_preference_finish (client, res, &error);
    if (!output) {
        mm_dbg ("QMI operation failed: %s", error->message);
        g_error_free (error);
    } else if (!qmi_message_nas_get_technology_preference_output_get_result (output, &error)) {
        mm_dbg ("Couldn't get technology preference: %s", error->message);
        g_error_free (error);
    } else {
        MMModemMode allowed;
        QmiNasRadioTechnologyPreference preference_mask;

        qmi_message_nas_get_technology_preference_output_get_active (
            output,
            &preference_mask,
            NULL, /* duration */
            NULL);
        allowed = modem_mode_from_qmi_radio_technology_preference (preference_mask);
        if (allowed == MM_MODEM_MODE_NONE) {
            gchar *str;

            str = qmi_nas_radio_technology_preference_build_string_from_mask (preference_mask);
            mm_dbg ("Unsupported modes reported: '%s'", str);
            g_free (str);
        } else {
            /* We got a valid value from here */
            result = g_new (LoadAllowedModesResult, 1);
            result->allowed = allowed;
            result->preferred = MM_MODEM_MODE_NONE;
        }
    }

    if (output)
        qmi_message_nas_get_technology_preference_output_unref (output);

    if (!result) {
        ctx->run_get_technology_preference = FALSE;
        load_allowed_modes_context_step (ctx);
        return;
    }

    g_simple_async_result_set_op_res_gpointer (
        ctx->result,
        result,
        (GDestroyNotify)g_free);
    load_allowed_modes_context_complete_and_free (ctx);
}

static MMModemMode
modem_mode_from_qmi_rat_mode_preference (QmiNasRatModePreference qmi)
{
    MMModemMode mode = MM_MODEM_MODE_NONE;

    if (qmi & QMI_NAS_RAT_MODE_PREFERENCE_CDMA_1X)
        mode |= MM_MODEM_MODE_2G;

    if (qmi & QMI_NAS_RAT_MODE_PREFERENCE_CDMA_1XEVDO)
        mode |= MM_MODEM_MODE_3G;

    if (qmi & QMI_NAS_RAT_MODE_PREFERENCE_GSM)
        mode |= MM_MODEM_MODE_2G;

    if (qmi & QMI_NAS_RAT_MODE_PREFERENCE_UMTS)
        mode |= MM_MODEM_MODE_3G;

    if (qmi & QMI_NAS_RAT_MODE_PREFERENCE_LTE)
        mode |= MM_MODEM_MODE_4G;

    /* Assume CS if 2G supported */
    if (mode & MM_MODEM_MODE_2G)
        mode |= MM_MODEM_MODE_CS;

    return mode;
}

static MMModemMode
modem_mode_from_qmi_gsm_wcdma_acquisition_order_preference (QmiNasGsmWcdmaAcquisitionOrderPreference qmi)
{
    switch (qmi) {
    case QMI_NAS_GSM_WCDMA_ACQUISITION_ORDER_PREFERENCE_AUTOMATIC:
        return MM_MODEM_MODE_NONE;
    case QMI_NAS_GSM_WCDMA_ACQUISITION_ORDER_PREFERENCE_GSM:
        return MM_MODEM_MODE_2G;
    case QMI_NAS_GSM_WCDMA_ACQUISITION_ORDER_PREFERENCE_WCDMA:
        return MM_MODEM_MODE_3G;
    default:
        mm_dbg ("Unknown acquisition order preference: '%s'",
                qmi_nas_gsm_wcdma_acquisition_order_preference_get_string (qmi));
        return MM_MODEM_MODE_NONE;
    }
}

static void
allowed_modes_get_system_selection_preference_ready (QmiClientNas *client,
                                                     GAsyncResult *res,
                                                     LoadAllowedModesContext *ctx)
{
    LoadAllowedModesResult *result = NULL;
    QmiMessageNasGetSystemSelectionPreferenceOutput *output = NULL;
    GError *error = NULL;
    QmiNasRatModePreference mode_preference_mask = 0;

    output = qmi_client_nas_get_system_selection_preference_finish (client, res, &error);
    if (!output) {
        mm_dbg ("QMI operation failed: %s", error->message);
        g_error_free (error);
    } else if (!qmi_message_nas_get_system_selection_preference_output_get_result (output, &error)) {
        mm_dbg ("Couldn't get system selection preference: %s", error->message);
        g_error_free (error);
    } else if (!qmi_message_nas_get_system_selection_preference_output_get_mode_preference (
                   output,
                   &mode_preference_mask,
                   NULL)) {
        /* Assuming here that Get System Selection Preference reports *always* all
         * optional fields that the current message version supports */
        ctx->self->priv->has_mode_preference_in_system_selection_preference = FALSE;
        mm_dbg ("Mode preference not reported in system selection preference");
    } else {
        MMModemMode allowed;

        allowed = modem_mode_from_qmi_rat_mode_preference (mode_preference_mask);
        if (allowed == MM_MODEM_MODE_NONE) {
            gchar *str;

            str = qmi_nas_rat_mode_preference_build_string_from_mask (mode_preference_mask);
            mm_dbg ("Unsupported modes reported: '%s'", str);
            g_free (str);
        } else {
            QmiNasGsmWcdmaAcquisitionOrderPreference gsm_or_wcdma;

            /* We got a valid value from here */
            result = g_new (LoadAllowedModesResult, 1);
            result->allowed = allowed;
            result->preferred = MM_MODEM_MODE_NONE;

            if (mode_preference_mask & QMI_NAS_RAT_MODE_PREFERENCE_GSM &&
                mode_preference_mask & QMI_NAS_RAT_MODE_PREFERENCE_UMTS &&
                qmi_message_nas_get_system_selection_preference_output_get_gsm_wcdma_acquisition_order_preference (
                        output,
                        &gsm_or_wcdma,
                        NULL)) {
                result->preferred = modem_mode_from_qmi_gsm_wcdma_acquisition_order_preference (gsm_or_wcdma);
            }
        }
    }

    if (output)
        qmi_message_nas_get_system_selection_preference_output_unref (output);

    if (!result) {
        /* Try with the deprecated command */
        ctx->run_get_system_selection_preference = FALSE;
        load_allowed_modes_context_step (ctx);
        return;
    }

    g_simple_async_result_set_op_res_gpointer (
        ctx->result,
        result,
        (GDestroyNotify)g_free);
    load_allowed_modes_context_complete_and_free (ctx);
}

static void
load_allowed_modes_context_step (LoadAllowedModesContext *ctx)
{
    if (ctx->run_get_system_selection_preference) {
        qmi_client_nas_get_system_selection_preference (
            ctx->client,
            NULL, /* no input */
            5,
            NULL, /* cancellable */
            (GAsyncReadyCallback)allowed_modes_get_system_selection_preference_ready,
            ctx);
        return;
    }

    if (ctx->run_get_technology_preference) {
        qmi_client_nas_get_technology_preference (
            ctx->client,
            NULL, /* no input */
            5,
            NULL, /* cancellable */
            (GAsyncReadyCallback)get_technology_preference_ready,
            ctx);
        return;
    }

    g_simple_async_result_set_error (
        ctx->result,
        MM_CORE_ERROR,
        MM_CORE_ERROR_UNSUPPORTED,
        "Loading allowed modes is not supported by this device");
    load_allowed_modes_context_complete_and_free (ctx);
}

static void
load_allowed_modes (MMIfaceModem *self,
                    GAsyncReadyCallback callback,
                    gpointer user_data)
{
    LoadAllowedModesContext *ctx;
    QmiClient *client = NULL;

    if (!ensure_qmi_client (MM_BROADBAND_MODEM_QMI (self),
                            QMI_SERVICE_NAS, &client,
                            callback, user_data))
        return;

    ctx = g_new0 (LoadAllowedModesContext, 1);
    ctx->self = g_object_ref (self);
    ctx->client = g_object_ref (client);
    ctx->result = g_simple_async_result_new (G_OBJECT (self),
                                             callback,
                                             user_data,
                                             load_allowed_modes);

    /* System selection preference introduced in NAS 1.1
     * TODO: Not sure when the System Selection Preference got the
     * 'Mode Preference' TLV, so we'll need to handle the fallback. */
    ctx->run_get_system_selection_preference =
        (qmi_client_check_version (client, 1, 1) &&
         ctx->self->priv->has_mode_preference_in_system_selection_preference);
    /* Technology preference introduced in NAS 1.7 */
    ctx->run_get_technology_preference = qmi_client_check_version (client, 1, 7);

    load_allowed_modes_context_step (ctx);
}

/*****************************************************************************/
/* Set allowed modes (Modem interface) */

typedef struct {
    MMBroadbandModemQmi *self;
    QmiClientNas *client;
    GSimpleAsyncResult *result;
    MMModemMode allowed;
    MMModemMode preferred;
    gboolean run_set_system_selection_preference;
    gboolean run_set_technology_preference;
} SetAllowedModesContext;

static void
set_allowed_modes_context_complete_and_free (SetAllowedModesContext *ctx)
{
    g_simple_async_result_complete_in_idle (ctx->result);
    g_object_unref (ctx->result);
    g_object_unref (ctx->client);
    g_object_unref (ctx->self);
    g_free (ctx);
}

static gboolean
set_allowed_modes_finish (MMIfaceModem *self,
                          GAsyncResult *res,
                          GError **error)
{
    return !g_simple_async_result_propagate_error (G_SIMPLE_ASYNC_RESULT (res), error);
}

static void set_allowed_modes_context_step (SetAllowedModesContext *ctx);

static void
set_technology_preference_ready (QmiClientNas *client,
                                 GAsyncResult *res,
                                 SetAllowedModesContext *ctx)
{
    QmiMessageNasSetTechnologyPreferenceOutput *output = NULL;
    GError *error = NULL;

    output = qmi_client_nas_set_technology_preference_finish (client, res, &error);
    if (!output) {
        mm_dbg ("QMI operation failed: %s", error->message);
        g_error_free (error);
    } else if (!qmi_message_nas_set_technology_preference_output_get_result (output, &error)) {
        mm_dbg ("Couldn't set technology preference: %s", error->message);
        g_error_free (error);
        qmi_message_nas_set_technology_preference_output_unref (output);
    } else {
        g_simple_async_result_set_op_res_gboolean (ctx->result, TRUE);
        set_allowed_modes_context_complete_and_free (ctx);
        qmi_message_nas_set_technology_preference_output_unref (output);
        return;
    }

    ctx->run_set_technology_preference = FALSE;
    set_allowed_modes_context_step (ctx);
}

static void
allowed_modes_set_system_selection_preference_ready (QmiClientNas *client,
                                                     GAsyncResult *res,
                                                     SetAllowedModesContext *ctx)
{
    QmiMessageNasSetSystemSelectionPreferenceOutput *output = NULL;
    GError *error = NULL;

    output = qmi_client_nas_set_system_selection_preference_finish (client, res, &error);
    if (!output) {
        mm_dbg ("QMI operation failed: %s", error->message);
        g_error_free (error);
    } else if (!qmi_message_nas_set_system_selection_preference_output_get_result (output, &error)) {
        if (g_error_matches (error,
                             QMI_PROTOCOL_ERROR,
                             QMI_PROTOCOL_ERROR_DEVICE_UNSUPPORTED))
            ctx->self->priv->has_mode_preference_in_system_selection_preference = FALSE;
        mm_dbg ("Couldn't set system selection preference: %s", error->message);
        g_error_free (error);
        qmi_message_nas_set_system_selection_preference_output_unref (output);
    } else {
        /* Good! TODO: do we really need to wait for the indication? */
        g_simple_async_result_set_op_res_gboolean (ctx->result, TRUE);
        set_allowed_modes_context_complete_and_free (ctx);
        qmi_message_nas_set_system_selection_preference_output_unref (output);
        return;
    }

    /* Try with the deprecated command */
    ctx->run_set_system_selection_preference = FALSE;
    set_allowed_modes_context_step (ctx);
}

static QmiNasRatModePreference
modem_mode_to_qmi_radio_technology_preference (MMModemMode mode,
                                               gboolean is_cdma)
{
    QmiNasRatModePreference pref = 0;

    if (is_cdma) {
        pref |= QMI_NAS_RADIO_TECHNOLOGY_PREFERENCE_3GPP2;
        if (mode & MM_MODEM_MODE_2G)
            pref |= QMI_NAS_RADIO_TECHNOLOGY_PREFERENCE_CDMA_OR_WCDMA; /* CDMA */
        if (mode & MM_MODEM_MODE_3G)
            pref |= QMI_NAS_RADIO_TECHNOLOGY_PREFERENCE_HDR; /* EV-DO */
    } else {
        pref |= QMI_NAS_RADIO_TECHNOLOGY_PREFERENCE_3GPP;
        if (mode & MM_MODEM_MODE_2G)
            pref |= QMI_NAS_RADIO_TECHNOLOGY_PREFERENCE_AMPS_OR_GSM; /* GSM */
        if (mode & MM_MODEM_MODE_3G)
            pref |= QMI_NAS_RADIO_TECHNOLOGY_PREFERENCE_CDMA_OR_WCDMA; /* WCDMA */
    }

    if (mode & MM_MODEM_MODE_4G)
        pref |= QMI_NAS_RADIO_TECHNOLOGY_PREFERENCE_LTE;

    return pref;
}

static QmiNasRatModePreference
modem_mode_to_qmi_rat_mode_preference (MMModemMode mode,
                                       gboolean is_cdma,
                                       gboolean is_3gpp)
{
    QmiNasRatModePreference pref = 0;

    if (is_cdma) {
        if (mode & MM_MODEM_MODE_2G)
            pref |= QMI_NAS_RAT_MODE_PREFERENCE_CDMA_1X;

        if (mode & MM_MODEM_MODE_3G)
            pref |= QMI_NAS_RAT_MODE_PREFERENCE_CDMA_1XEVDO;
    }

    if (is_3gpp) {
        if (mode & MM_MODEM_MODE_2G)
            pref |= QMI_NAS_RAT_MODE_PREFERENCE_GSM;

        if (mode & MM_MODEM_MODE_3G)
            pref |= QMI_NAS_RAT_MODE_PREFERENCE_UMTS;

        if (mode & MM_MODEM_MODE_4G)
            pref |= QMI_NAS_RAT_MODE_PREFERENCE_LTE;
    }

    return mode;
}

static QmiNasGsmWcdmaAcquisitionOrderPreference
modem_mode_to_qmi_gsm_wcdma_acquisition_order_preference (MMModemMode mode)
{
    gchar *str;

    /* mode is not a mask in this case, only a value */

    switch (mode) {
    case MM_MODEM_MODE_3G:
        return QMI_NAS_GSM_WCDMA_ACQUISITION_ORDER_PREFERENCE_WCDMA;
    case MM_MODEM_MODE_2G:
        return QMI_NAS_GSM_WCDMA_ACQUISITION_ORDER_PREFERENCE_GSM;
    case MM_MODEM_MODE_NONE:
        return QMI_NAS_GSM_WCDMA_ACQUISITION_ORDER_PREFERENCE_AUTOMATIC;
    default:
        break;
    }

    str = mm_modem_mode_build_string_from_mask (mode);
    mm_dbg ("Unhandled modem mode: '%s'", str);
    g_free (str);
    return MM_MODEM_MODE_NONE;
}

static void
set_allowed_modes_context_step (SetAllowedModesContext *ctx)
{
    if (ctx->run_set_system_selection_preference) {
        QmiMessageNasSetSystemSelectionPreferenceInput *input;
        QmiNasRatModePreference pref;

        pref = modem_mode_to_qmi_rat_mode_preference (ctx->allowed,
                                                      mm_iface_modem_is_cdma (MM_IFACE_MODEM (ctx->self)),
                                                      mm_iface_modem_is_3gpp (MM_IFACE_MODEM (ctx->self)));
        if (!pref) {
            gchar *str;

            str = mm_modem_mode_build_string_from_mask (ctx->allowed);
            g_simple_async_result_set_error (ctx->result,
                                             MM_CORE_ERROR,
                                             MM_CORE_ERROR_FAILED,
                                             "Unhandled allowed mode setting: '%s'",
                                             str);
            g_free (str);
            set_allowed_modes_context_complete_and_free (ctx);
            return;
        }

        input = qmi_message_nas_set_system_selection_preference_input_new ();
        qmi_message_nas_set_system_selection_preference_input_set_mode_preference (input, pref, NULL);

        if (mm_iface_modem_is_3gpp (MM_IFACE_MODEM (ctx->self))) {
            QmiNasGsmWcdmaAcquisitionOrderPreference order;

            order = modem_mode_to_qmi_gsm_wcdma_acquisition_order_preference (ctx->preferred);
            qmi_message_nas_set_system_selection_preference_input_set_gsm_wcdma_acquisition_order_preference (input, order, NULL);
        }

        qmi_message_nas_set_system_selection_preference_input_set_change_duration (input, QMI_NAS_CHANGE_DURATION_PERMANENT, NULL);

        qmi_client_nas_set_system_selection_preference (
            ctx->client,
            input,
            5,
            NULL, /* cancellable */
            (GAsyncReadyCallback)allowed_modes_set_system_selection_preference_ready,
            ctx);
        qmi_message_nas_set_system_selection_preference_input_unref (input);
        return;
    }

    if (ctx->run_set_technology_preference) {
        QmiMessageNasSetTechnologyPreferenceInput *input;
        QmiNasRadioTechnologyPreference pref;

        if (ctx->preferred != MM_MODEM_MODE_NONE) {
            g_simple_async_result_set_error (ctx->result,
                                             MM_CORE_ERROR,
                                             MM_CORE_ERROR_FAILED,
                                             "Cannot set specific preferred mode");
            set_allowed_modes_context_complete_and_free (ctx);
            return;
        }

        pref = modem_mode_to_qmi_radio_technology_preference (ctx->allowed,
                                                              mm_iface_modem_is_cdma (MM_IFACE_MODEM (ctx->self)));
        if (!pref) {
            gchar *str;

            str = mm_modem_mode_build_string_from_mask (ctx->allowed);
            g_simple_async_result_set_error (ctx->result,
                                             MM_CORE_ERROR,
                                             MM_CORE_ERROR_FAILED,
                                             "Unhandled allowed mode setting: '%s'",
                                             str);
            g_free (str);
            set_allowed_modes_context_complete_and_free (ctx);
            return;
        }

        input = qmi_message_nas_set_technology_preference_input_new ();
        qmi_message_nas_set_technology_preference_input_set_current (input, pref, QMI_NAS_PREFERENCE_DURATION_PERMANENT, NULL);

        qmi_client_nas_set_technology_preference (
            ctx->client,
            input,
            5,
            NULL, /* cancellable */
            (GAsyncReadyCallback)set_technology_preference_ready,
            ctx);
        qmi_message_nas_set_technology_preference_input_unref (input);
        return;
    }

    g_simple_async_result_set_error (
        ctx->result,
        MM_CORE_ERROR,
        MM_CORE_ERROR_UNSUPPORTED,
        "Setting allowed modes is not supported by this device");
    set_allowed_modes_context_complete_and_free (ctx);
}

static void
set_allowed_modes (MMIfaceModem *self,
                   MMModemMode allowed,
                   MMModemMode preferred,
                   GAsyncReadyCallback callback,
                   gpointer user_data)
{
    SetAllowedModesContext *ctx;
    QmiClient *client = NULL;

    if (!ensure_qmi_client (MM_BROADBAND_MODEM_QMI (self),
                            QMI_SERVICE_NAS, &client,
                            callback, user_data))
        return;

    ctx = g_new0 (SetAllowedModesContext, 1);
    ctx->self = g_object_ref (self);
    ctx->client = g_object_ref (client);
    ctx->result = g_simple_async_result_new (G_OBJECT (self),
                                             callback,
                                             user_data,
                                             set_allowed_modes);
    ctx->allowed = allowed;
    ctx->preferred = preferred;
    /* System selection preference introduced in NAS 1.1
     * TODO: Not sure when the System Selection Preference got the
     * 'Mode Preference' TLV, so we'll need to handle the fallback. */
    ctx->run_set_system_selection_preference =
        (qmi_client_check_version (client, 1, 1) &&
         ctx->self->priv->has_mode_preference_in_system_selection_preference);
    /* Technology preference introduced in NAS 1.7 */
    ctx->run_set_technology_preference = qmi_client_check_version (client, 1, 7);

    set_allowed_modes_context_step (ctx);
}

/*****************************************************************************/
/* IMEI loading (3GPP interface) */

static gchar *
modem_3gpp_load_imei_finish (MMIfaceModem3gpp *self,
                             GAsyncResult *res,
                             GError **error)
{
    gchar *imei;

    if (g_simple_async_result_propagate_error (G_SIMPLE_ASYNC_RESULT (res), error))
        return NULL;

    imei = g_strdup (g_simple_async_result_get_op_res_gpointer (G_SIMPLE_ASYNC_RESULT (res)));
    mm_dbg ("loaded IMEI: %s", imei);
    return imei;
}

static void
modem_3gpp_load_imei (MMIfaceModem3gpp *_self,
                      GAsyncReadyCallback callback,
                      gpointer user_data)
{
    MMBroadbandModemQmi *self = MM_BROADBAND_MODEM_QMI (_self);
    GSimpleAsyncResult *result;

    result = g_simple_async_result_new (G_OBJECT (self),
                                        callback,
                                        user_data,
                                        modem_3gpp_load_imei);

    if (self->priv->imei)
        g_simple_async_result_set_op_res_gpointer (result,
                                                   self->priv->imei,
                                                   NULL);
    else
        g_simple_async_result_set_error (result,
                                         MM_CORE_ERROR,
                                         MM_CORE_ERROR_FAILED,
                                         "Device doesn't report a valid IMEI");
    g_simple_async_result_complete_in_idle (result);
    g_object_unref (result);
}

/*****************************************************************************/
/* Facility locks status loading (3GPP interface) */

typedef struct {
    MMBroadbandModem *self;
    GSimpleAsyncResult *result;
    QmiClient *client;
    guint current;
    MMModem3gppFacility facilities;
    MMModem3gppFacility locks;
} LoadEnabledFacilityLocksContext;

static void get_next_facility_lock_status (LoadEnabledFacilityLocksContext *ctx);

static void
load_enabled_facility_locks_context_complete_and_free (LoadEnabledFacilityLocksContext *ctx)
{
    g_simple_async_result_complete (ctx->result);
    g_object_unref (ctx->result);
    g_object_unref (ctx->client);
    g_object_unref (ctx->self);
    g_free (ctx);
}

static MMModem3gppFacility
modem_3gpp_load_enabled_facility_locks_finish (MMIfaceModem3gpp *self,
                                               GAsyncResult *res,
                                               GError **error)
{
    if (g_simple_async_result_propagate_error (G_SIMPLE_ASYNC_RESULT (res), error))
        return MM_MODEM_3GPP_FACILITY_NONE;

    return ((MMModem3gppFacility) GPOINTER_TO_UINT (
                g_simple_async_result_get_op_res_gpointer (G_SIMPLE_ASYNC_RESULT (res))));
}

static QmiDmsUimFacility
get_qmi_facility_from_mm_facility (MMModem3gppFacility mm)
{
    switch (mm) {
    case MM_MODEM_3GPP_FACILITY_PH_SIM:
        /* Not really sure about this one; it may be PH_FSIM? */
        return QMI_DMS_UIM_FACILITY_PF;

    case MM_MODEM_3GPP_FACILITY_NET_PERS:
        return QMI_DMS_UIM_FACILITY_PN;

    case MM_MODEM_3GPP_FACILITY_NET_SUB_PERS:
        return QMI_DMS_UIM_FACILITY_PU;

    case MM_MODEM_3GPP_FACILITY_PROVIDER_PERS:
        return QMI_DMS_UIM_FACILITY_PP;

    case MM_MODEM_3GPP_FACILITY_CORP_PERS:
        return QMI_DMS_UIM_FACILITY_PC;

    default:
        /* Never try to ask for a facility we cannot translate */
        g_assert_not_reached ();
    }
}

static void
dms_uim_get_ck_status_ready (QmiClientDms *client,
                             GAsyncResult *res,
                             LoadEnabledFacilityLocksContext *ctx)
{
    gchar *facility_str;
    QmiMessageDmsUimGetCkStatusOutput *output;

    facility_str = mm_modem_3gpp_facility_build_string_from_mask (1 << ctx->current);
    output = qmi_client_dms_uim_get_ck_status_finish (client, res, NULL);
    if (!output ||
        !qmi_message_dms_uim_get_ck_status_output_get_result (output, NULL)) {
        /* On errors, we'll just assume disabled */
        mm_dbg ("Couldn't query facility '%s' status, assuming disabled", facility_str);
        ctx->locks &= ~(1 << ctx->current);
    } else {
        QmiDmsUimFacilityState state;
        guint8 verify_retries_left;
        guint8 unblock_retries_left;

        qmi_message_dms_uim_get_ck_status_output_get_ck_status (
            output,
            &state,
            &verify_retries_left,
            &unblock_retries_left,
            NULL);

        mm_dbg ("Facility '%s' is: '%s'",
                facility_str,
                qmi_dms_uim_facility_state_get_string (state));

        if (state == QMI_DMS_UIM_FACILITY_STATE_ACTIVATED ||
            state == QMI_DMS_UIM_FACILITY_STATE_BLOCKED) {
            ctx->locks |= (1 << ctx->current);
        }
    }

    if (output)
        qmi_message_dms_uim_get_ck_status_output_unref (output);
    g_free (facility_str);

    /* And go on with the next one */
    ctx->current++;
    get_next_facility_lock_status (ctx);
}

static void
get_next_facility_lock_status (LoadEnabledFacilityLocksContext *ctx)
{
    guint i;

    for (i = ctx->current; i < sizeof (MMModem3gppFacility) * 8; i++) {
        guint32 facility = 1 << i;

        /* Found the next one to query! */
        if (ctx->facilities & facility) {
            QmiMessageDmsUimGetCkStatusInput *input;

            /* Keep the current one */
            ctx->current = i;

            /* Query current */
            input = qmi_message_dms_uim_get_ck_status_input_new ();
            qmi_message_dms_uim_get_ck_status_input_set_facility (
                input,
                get_qmi_facility_from_mm_facility (facility),
                NULL);
            qmi_client_dms_uim_get_ck_status (QMI_CLIENT_DMS (ctx->client),
                                              input,
                                              5,
                                              NULL,
                                              (GAsyncReadyCallback)dms_uim_get_ck_status_ready,
                                              ctx);
            qmi_message_dms_uim_get_ck_status_input_unref (input);
            return;
        }
    }

    /* No more facilities to query, all done */
    g_simple_async_result_set_op_res_gpointer (ctx->result,
                                               GUINT_TO_POINTER (ctx->locks),
                                               NULL);
    load_enabled_facility_locks_context_complete_and_free (ctx);
}

static void
modem_3gpp_load_enabled_facility_locks (MMIfaceModem3gpp *self,
                                        GAsyncReadyCallback callback,
                                        gpointer user_data)
{
    LoadEnabledFacilityLocksContext *ctx;
    QmiClient *client = NULL;

    if (!ensure_qmi_client (MM_BROADBAND_MODEM_QMI (self),
                            QMI_SERVICE_DMS, &client,
                            callback, user_data))
        return;

    ctx = g_new (LoadEnabledFacilityLocksContext, 1);
    ctx->self = g_object_ref (self);
    ctx->client = g_object_ref (client);
    ctx->result = g_simple_async_result_new (G_OBJECT (self),
                                             callback,
                                             user_data,
                                             modem_3gpp_load_enabled_facility_locks);
    /* Set initial list of facilities to query */
    ctx->facilities = (MM_MODEM_3GPP_FACILITY_PH_SIM |
                       MM_MODEM_3GPP_FACILITY_NET_PERS |
                       MM_MODEM_3GPP_FACILITY_NET_SUB_PERS |
                       MM_MODEM_3GPP_FACILITY_PROVIDER_PERS |
                       MM_MODEM_3GPP_FACILITY_CORP_PERS);
    ctx->locks = MM_MODEM_3GPP_FACILITY_NONE;
    ctx->current = 0;

    get_next_facility_lock_status (ctx);
}

/*****************************************************************************/
/* Scan networks (3GPP interface) */

static GList *
modem_3gpp_scan_networks_finish (MMIfaceModem3gpp *self,
                                 GAsyncResult *res,
                                 GError **error)
{
    if (g_simple_async_result_propagate_error (G_SIMPLE_ASYNC_RESULT (res), error))
        return NULL;

    /* We return the GList as it is */
    return (GList *) g_simple_async_result_get_op_res_gpointer (G_SIMPLE_ASYNC_RESULT (res));
}

static MMModem3gppNetworkAvailability
network_availability_from_qmi_nas_network_status (QmiNasNetworkStatus qmi)
{
    if (qmi & QMI_NAS_NETWORK_STATUS_CURRENT_SERVING)
        return MM_MODEM_3GPP_NETWORK_AVAILABILITY_CURRENT;

    if (qmi & QMI_NAS_NETWORK_STATUS_AVAILABLE) {
        if (qmi & QMI_NAS_NETWORK_STATUS_FORBIDDEN)
            return MM_MODEM_3GPP_NETWORK_AVAILABILITY_FORBIDDEN;
        return MM_MODEM_3GPP_NETWORK_AVAILABILITY_AVAILABLE;
    }

    return MM_MODEM_3GPP_NETWORK_AVAILABILITY_UNKNOWN;
}

static MM3gppNetworkInfo *
get_3gpp_network_info (QmiMessageNasNetworkScanOutputNetworkInformationElement *element)
{
    GString *aux;
    MM3gppNetworkInfo *info;

    info = g_new (MM3gppNetworkInfo, 1);
    info->status = network_availability_from_qmi_nas_network_status (element->network_status);

    aux = g_string_new ("");
    /* MCC always 3 digits */
    g_string_append_printf (aux, "%.3"G_GUINT16_FORMAT, element->mcc);
    /* Guess about MNC, if < 100 assume it's 2 digits, no PCS info here */
    if (element->mnc >= 100)
        g_string_append_printf (aux, "%.3"G_GUINT16_FORMAT, element->mnc);
    else
        g_string_append_printf (aux, "%.2"G_GUINT16_FORMAT, element->mnc);

    info->operator_code = g_string_free (aux, FALSE);
    info->operator_short = NULL;
    info->operator_long = g_strdup (element->description);
    info->access_tech = MM_MODEM_ACCESS_TECHNOLOGY_UNKNOWN;

    return info;
}

static MMModemAccessTechnology
access_technology_from_qmi_rat (QmiNasRadioInterface interface)
{
    switch (interface) {
    case QMI_NAS_RADIO_INTERFACE_CDMA_1X:
        return MM_MODEM_ACCESS_TECHNOLOGY_1XRTT;
    case QMI_NAS_RADIO_INTERFACE_CDMA_1XEVDO:
        return MM_MODEM_ACCESS_TECHNOLOGY_EVDO0;
    case QMI_NAS_RADIO_INTERFACE_GSM:
        return MM_MODEM_ACCESS_TECHNOLOGY_GSM;
    case QMI_NAS_RADIO_INTERFACE_UMTS:
        return MM_MODEM_ACCESS_TECHNOLOGY_UMTS;
    case QMI_NAS_RADIO_INTERFACE_LTE:
        return MM_MODEM_ACCESS_TECHNOLOGY_LTE;
    case QMI_NAS_RADIO_INTERFACE_TD_SCDMA:
    case QMI_NAS_RADIO_INTERFACE_AMPS:
    case QMI_NAS_RADIO_INTERFACE_NONE:
    default:
        return MM_MODEM_ACCESS_TECHNOLOGY_UNKNOWN;
    }
}

static MMModemAccessTechnology
get_3gpp_access_technology (GArray *array,
                            gboolean *array_used_flags,
                            guint16 mcc,
                            guint16 mnc)
{
    guint i;

    for (i = 0; i < array->len; i++) {
        QmiMessageNasNetworkScanOutputRadioAccessTechnologyElement *element;

        if (array_used_flags[i])
            continue;

        element = &g_array_index (array, QmiMessageNasNetworkScanOutputRadioAccessTechnologyElement, i);
        if (element->mcc == mcc &&
            element->mnc == mnc) {
            array_used_flags[i] = TRUE;
            return access_technology_from_qmi_rat (element->radio_interface);
        }
    }

    return MM_MODEM_ACCESS_TECHNOLOGY_UNKNOWN;
}

static void
nas_network_scan_ready (QmiClientNas *client,
                        GAsyncResult *res,
                        GSimpleAsyncResult *simple)
{
    QmiMessageNasNetworkScanOutput *output = NULL;
    GError *error = NULL;

    output = qmi_client_nas_network_scan_finish (client, res, &error);
    if (!output) {
        g_prefix_error (&error, "QMI operation failed: ");
        g_simple_async_result_take_error (simple, error);
    } else if (!qmi_message_nas_network_scan_output_get_result (output, &error)) {
        g_prefix_error (&error, "Couldn't scan networks: ");
        g_simple_async_result_take_error (simple, error);
    } else {
        GList *scan_result = NULL;
        GArray *info_array = NULL;

        if (qmi_message_nas_network_scan_output_get_network_information (output, &info_array, NULL)) {
            GArray *rat_array = NULL;
            gboolean *rat_array_used_flags = NULL;
            guint i;

            /* Get optional RAT array */
            qmi_message_nas_network_scan_output_get_radio_access_technology (output, &rat_array, NULL);
            if (rat_array)
                rat_array_used_flags = g_new0 (gboolean, rat_array->len);

            for (i = 0; i < info_array->len; i++) {
                QmiMessageNasNetworkScanOutputNetworkInformationElement *info_element;
                MM3gppNetworkInfo *info;

                info_element = &g_array_index (info_array, QmiMessageNasNetworkScanOutputNetworkInformationElement, i);

                info = get_3gpp_network_info (info_element);
                if (rat_array)
                    info->access_tech = get_3gpp_access_technology (rat_array,
                                                                    rat_array_used_flags,
                                                                    info_element->mcc,
                                                                    info_element->mnc);

                scan_result = g_list_append (scan_result, info);
            }

            g_free (rat_array_used_flags);
        }

        /* We *require* a callback in the async method, as we're not setting a
         * GDestroyNotify callback */
        g_simple_async_result_set_op_res_gpointer (simple, scan_result, NULL);
    }

    if (output)
        qmi_message_nas_network_scan_output_unref (output);

    g_simple_async_result_complete (simple);
    g_object_unref (simple);
}

static void
modem_3gpp_scan_networks (MMIfaceModem3gpp *self,
                          GAsyncReadyCallback callback,
                          gpointer user_data)
{
    GSimpleAsyncResult *result;
    QmiClient *client = NULL;

    /* We will pass the GList in the GSimpleAsyncResult, so we must
     * ensure that there is a callback so that we get it properly
     * passed to the caller and deallocated afterwards */
    g_assert (callback != NULL);

    if (!ensure_qmi_client (MM_BROADBAND_MODEM_QMI (self),
                            QMI_SERVICE_NAS, &client,
                            callback, user_data))
        return;

    result = g_simple_async_result_new (G_OBJECT (self),
                                        callback,
                                        user_data,
                                        modem_3gpp_scan_networks);

    mm_dbg ("Scanning networks...");
    qmi_client_nas_network_scan (QMI_CLIENT_NAS (client),
                                 NULL,
                                 100,
                                 NULL,
                                 (GAsyncReadyCallback)nas_network_scan_ready,
                                 result);
}

/*****************************************************************************/
/* Load operator name (3GPP interface) */

static gchar *
modem_3gpp_load_operator_name_finish (MMIfaceModem3gpp *_self,
                                      GAsyncResult *res,
                                      GError **error)
{
    MMBroadbandModemQmi *self = MM_BROADBAND_MODEM_QMI (_self);

    if (self->priv->current_operator_description)
        return g_strdup (self->priv->current_operator_description);

    g_set_error (error,
                 MM_CORE_ERROR,
                 MM_CORE_ERROR_FAILED,
                 "Current operator description is still unknown");
    return NULL;
}

static void
modem_3gpp_load_operator_name (MMIfaceModem3gpp *self,
                               GAsyncReadyCallback callback,
                               gpointer user_data)
{
    GSimpleAsyncResult *result;

    /* Just finish the async operation */
    result = g_simple_async_result_new (G_OBJECT (self),
                                        callback,
                                        user_data,
                                        modem_3gpp_load_operator_name);
    g_simple_async_result_set_op_res_gboolean (result, TRUE);
    g_simple_async_result_complete_in_idle (result);
    g_object_unref (result);
}

/*****************************************************************************/
/* Load operator code (3GPP interface) */

static gchar *
modem_3gpp_load_operator_code_finish (MMIfaceModem3gpp *_self,
                                      GAsyncResult *res,
                                      GError **error)
{
    MMBroadbandModemQmi *self = MM_BROADBAND_MODEM_QMI (_self);

    if (self->priv->current_operator_id)
        return g_strdup (self->priv->current_operator_id);

    g_set_error (error,
                 MM_CORE_ERROR,
                 MM_CORE_ERROR_FAILED,
                 "Current operator MCC/MNC is still unknown");
    return NULL;
}

static void
modem_3gpp_load_operator_code (MMIfaceModem3gpp *self,
                               GAsyncReadyCallback callback,
                               gpointer user_data)
{
    GSimpleAsyncResult *result;

    /* Just finish the async operation */
    result = g_simple_async_result_new (G_OBJECT (self),
                                        callback,
                                        user_data,
                                        modem_3gpp_load_operator_code);
    g_simple_async_result_set_op_res_gboolean (result, TRUE);
    g_simple_async_result_complete_in_idle (result);
    g_object_unref (result);
}

/*****************************************************************************/
/* Register in network (3GPP interface) */

static gboolean
modem_3gpp_register_in_network_finish (MMIfaceModem3gpp *self,
                                       GAsyncResult *res,
                                       GError **error)
{
    return !g_simple_async_result_propagate_error (G_SIMPLE_ASYNC_RESULT (res), error);
}

static void
initiate_network_register_ready (QmiClientNas *client,
                                 GAsyncResult *res,
                                 GSimpleAsyncResult *simple)
{
    GError *error = NULL;
    QmiMessageNasInitiateNetworkRegisterOutput *output;

    output = qmi_client_nas_initiate_network_register_finish (client, res, &error);
    if (!output) {
        g_prefix_error (&error, "QMI operation failed: ");
        g_simple_async_result_take_error (simple, error);
    } else if (!qmi_message_nas_initiate_network_register_output_get_result (output, &error)) {
        /* NOFX is not an error, they actually play pretty well */
        if (g_error_matches (error,
                             QMI_PROTOCOL_ERROR,
                             QMI_PROTOCOL_ERROR_NO_EFFECT)) {
            g_error_free (error);
            g_simple_async_result_set_op_res_gboolean (simple, TRUE);
        } else {
            g_prefix_error (&error, "Couldn't initiate network register: ");
            g_simple_async_result_take_error (simple, error);
        }
    } else
        g_simple_async_result_set_op_res_gboolean (simple, TRUE);

    if (output)
        qmi_message_nas_initiate_network_register_output_unref (output);
    g_simple_async_result_complete (simple);
    g_object_unref (simple);
}

static void
modem_3gpp_register_in_network (MMIfaceModem3gpp *self,
                                const gchar *operator_id,
                                GCancellable *cancellable,
                                GAsyncReadyCallback callback,
                                gpointer user_data)
{
    guint16 mcc = 0;
    guint16 mnc = 0;
    QmiClient *client = NULL;
    QmiMessageNasInitiateNetworkRegisterInput *input;
    GError *error = NULL;

    /* Parse input MCC/MNC */
    if (operator_id && !mm_3gpp_parse_operator_id (operator_id, &mcc, &mnc, &error)) {
        g_assert (error != NULL);
        g_simple_async_report_take_gerror_in_idle (G_OBJECT (self),
                                                   callback,
                                                   user_data,
                                                   error);
        return;
    }

    /* Get NAS client */
    if (!ensure_qmi_client (MM_BROADBAND_MODEM_QMI (self),
                            QMI_SERVICE_NAS, &client,
                            callback, user_data))
        return;

    input = qmi_message_nas_initiate_network_register_input_new ();

    if (mcc) {
        /* If the user sent a specific network to use, lock it in. */
        qmi_message_nas_initiate_network_register_input_set_action (
            input,
            QMI_NAS_NETWORK_REGISTER_TYPE_MANUAL,
            NULL);
        qmi_message_nas_initiate_network_register_input_set_manual_registration_info_3gpp (
            input,
            mcc,
            mnc,
            QMI_NAS_RADIO_INTERFACE_UNKNOWN,
            NULL);
    } else {
        /* Otherwise, automatic registration */
        qmi_message_nas_initiate_network_register_input_set_action (
            input,
            QMI_NAS_NETWORK_REGISTER_TYPE_AUTOMATIC,
            NULL);
    }

    qmi_client_nas_initiate_network_register (
        QMI_CLIENT_NAS (client),
        input,
        120,
        cancellable,
        (GAsyncReadyCallback)initiate_network_register_ready,
        g_simple_async_result_new (G_OBJECT (self),
                                   callback,
                                   user_data,
                                   modem_3gpp_register_in_network));

    qmi_message_nas_initiate_network_register_input_unref (input);
}

/*****************************************************************************/
/* Registration checks (3GPP interface) */

typedef struct {
    MMBroadbandModemQmi *self;
    QmiClientNas *client;
    GSimpleAsyncResult *result;
} Run3gppRegistrationChecksContext;

static void
run_3gpp_registration_checks_context_complete_and_free (Run3gppRegistrationChecksContext *ctx)
{
    g_simple_async_result_complete (ctx->result);
    g_object_unref (ctx->result);
    g_object_unref (ctx->client);
    g_object_unref (ctx->self);
    g_free (ctx);
}

static gboolean
modem_3gpp_run_registration_checks_finish (MMIfaceModem3gpp *self,
                                           GAsyncResult *res,
                                           GError **error)
{
    return !g_simple_async_result_propagate_error (G_SIMPLE_ASYNC_RESULT (res), error);
}

static MMModem3gppRegistrationState
qmi_registration_state_to_3gpp_registration_state (QmiNasAttachState attach_state,
                                                   QmiNasRegistrationState registration_state,
                                                   gboolean roaming)
{
    if (attach_state == QMI_NAS_ATTACH_STATE_UNKNOWN)
        return MM_MODEM_3GPP_REGISTRATION_STATE_UNKNOWN;

    if (attach_state == QMI_NAS_ATTACH_STATE_DETACHED)
        return QMI_NAS_REGISTRATION_STATE_NOT_REGISTERED;

    /* attached */

    switch (registration_state) {
    case QMI_NAS_REGISTRATION_STATE_NOT_REGISTERED:
        return MM_MODEM_3GPP_REGISTRATION_STATE_IDLE;
    case QMI_NAS_REGISTRATION_STATE_REGISTERED:
        return (roaming ? MM_MODEM_3GPP_REGISTRATION_STATE_ROAMING : MM_MODEM_3GPP_REGISTRATION_STATE_HOME);
    case QMI_NAS_REGISTRATION_STATE_NOT_REGISTERED_SEARCHING:
        return MM_MODEM_3GPP_REGISTRATION_STATE_SEARCHING;
    case QMI_NAS_REGISTRATION_STATE_REGISTRATION_DENIED:
        return MM_MODEM_3GPP_REGISTRATION_STATE_DENIED;
    case QMI_NAS_REGISTRATION_STATE_UNKNOWN:
    default:
        return MM_MODEM_3GPP_REGISTRATION_STATE_UNKNOWN;
    }
}

static MMModemAccessTechnology
qmi_radio_interface_list_to_access_technologies (GArray *radio_interfaces)
{
    MMModemAccessTechnology access_technology = 0;
    guint i;

    for (i = 0; i < radio_interfaces->len; i++) {
        QmiNasRadioInterface iface;

        iface = g_array_index (radio_interfaces, QmiNasRadioInterface, i);
        access_technology |= access_technology_from_qmi_rat (iface);
    }

    return access_technology;
}

static MMModemAccessTechnology
access_technology_from_qmi_data_capability (QmiNasDataCapability cap)
{
    switch (cap) {
    case QMI_NAS_DATA_CAPABILITY_GPRS:
        return MM_MODEM_ACCESS_TECHNOLOGY_GPRS;
    case QMI_NAS_DATA_CAPABILITY_EDGE:
        return MM_MODEM_ACCESS_TECHNOLOGY_EDGE;
    case QMI_NAS_DATA_CAPABILITY_HSDPA:
        return MM_MODEM_ACCESS_TECHNOLOGY_HSDPA;
    case QMI_NAS_DATA_CAPABILITY_HSUPA:
        return MM_MODEM_ACCESS_TECHNOLOGY_HSUPA;
    case QMI_NAS_DATA_CAPABILITY_WCDMA:
        return MM_MODEM_ACCESS_TECHNOLOGY_UMTS;
    case QMI_NAS_DATA_CAPABILITY_CDMA:
        return MM_MODEM_ACCESS_TECHNOLOGY_1XRTT;
    case QMI_NAS_DATA_CAPABILITY_EVDO_REV_0:
        return MM_MODEM_ACCESS_TECHNOLOGY_EVDO0;
    case QMI_NAS_DATA_CAPABILITY_EVDO_REV_A:
        return MM_MODEM_ACCESS_TECHNOLOGY_EVDOA;
    case QMI_NAS_DATA_CAPABILITY_GSM:
        return MM_MODEM_ACCESS_TECHNOLOGY_GSM;
    case QMI_NAS_DATA_CAPABILITY_EVDO_REV_B:
        return MM_MODEM_ACCESS_TECHNOLOGY_EVDOB;
    case QMI_NAS_DATA_CAPABILITY_LTE:
        return MM_MODEM_ACCESS_TECHNOLOGY_LTE;
    case QMI_NAS_DATA_CAPABILITY_HSDPA_PLUS:
    case QMI_NAS_DATA_CAPABILITY_DC_HSDPA_PLUS:
        return MM_MODEM_ACCESS_TECHNOLOGY_HSPA_PLUS;
    default:
        return MM_MODEM_ACCESS_TECHNOLOGY_UNKNOWN;
    }
}

static MMModemAccessTechnology
qmi_data_capability_list_to_access_technologies (GArray *data_capabilities)
{
    MMModemAccessTechnology access_technology = 0;
    guint i;

    for (i = 0; i < data_capabilities->len; i++) {
        QmiNasDataCapability cap;

        cap = g_array_index (data_capabilities, QmiNasDataCapability, i);
        access_technology |= access_technology_from_qmi_data_capability (cap);
    }

    return access_technology;
}

static void
common_process_serving_system_3gpp (MMBroadbandModemQmi *self,
                                    QmiMessageNasGetServingSystemOutput *response_output,
                                    QmiIndicationNasServingSystemOutput *indication_output)
{
    QmiNasRegistrationState registration_state;
    QmiNasAttachState cs_attach_state;
    QmiNasAttachState ps_attach_state;
    QmiNasNetworkType selected_network;
    GArray *radio_interfaces;
    GArray *data_service_capabilities;
    QmiNasRoamingIndicatorStatus roaming;
    guint16 mcc;
    guint16 mnc;
    const gchar *description;
    gboolean has_pcs_digit;
    guint16 lac;
    guint32 cid;
    MMModemAccessTechnology mm_access_technologies;
    MMModem3gppRegistrationState mm_cs_registration_state;
    MMModem3gppRegistrationState mm_ps_registration_state;

    if (response_output)
        qmi_message_nas_get_serving_system_output_get_serving_system (
            response_output,
            &registration_state,
            &cs_attach_state,
            &ps_attach_state,
            &selected_network,
            &radio_interfaces,
            NULL);
    else
        qmi_indication_nas_serving_system_output_get_serving_system (
            indication_output,
            &registration_state,
            &cs_attach_state,
            &ps_attach_state,
            &selected_network,
            &radio_interfaces,
            NULL);

    /* Only process 3GPP info */
    if (selected_network != QMI_NAS_NETWORK_TYPE_3GPP) {
        g_free (self->priv->current_operator_id);
        self->priv->current_operator_id = NULL;
        g_free (self->priv->current_operator_description);
        self->priv->current_operator_description = NULL;
        mm_iface_modem_3gpp_update_cs_registration_state (MM_IFACE_MODEM_3GPP (self), MM_MODEM_3GPP_REGISTRATION_STATE_UNKNOWN);
        mm_iface_modem_3gpp_update_ps_registration_state (MM_IFACE_MODEM_3GPP (self), MM_MODEM_3GPP_REGISTRATION_STATE_UNKNOWN);
        mm_iface_modem_3gpp_update_access_technologies (MM_IFACE_MODEM_3GPP (self), MM_MODEM_ACCESS_TECHNOLOGY_UNKNOWN);
        mm_iface_modem_3gpp_update_location (MM_IFACE_MODEM_3GPP (self), 0, 0);
        return;
    }

    /* Get roaming status.
     * TODO: QMI may report per-access-technology roaming indicators, for when
     * the modem is connected to more than one network. How to handle those? */
    roaming = QMI_NAS_ROAMING_INDICATOR_STATUS_OFF;
    if (response_output)
        qmi_message_nas_get_serving_system_output_get_roaming_indicator (response_output, &roaming, NULL);
    else
        qmi_indication_nas_serving_system_output_get_roaming_indicator (indication_output, &roaming, NULL);

    /* Build MM registration states */
    mm_cs_registration_state =
        qmi_registration_state_to_3gpp_registration_state (
            cs_attach_state,
            registration_state,
            (roaming == QMI_NAS_ROAMING_INDICATOR_STATUS_ON));
    mm_ps_registration_state =
        qmi_registration_state_to_3gpp_registration_state (
            ps_attach_state,
            registration_state,
            (roaming == QMI_NAS_ROAMING_INDICATOR_STATUS_ON));

    /* Build access technologies mask */
    data_service_capabilities = NULL;
    if (response_output)
        qmi_message_nas_get_serving_system_output_get_data_service_capability (response_output, &data_service_capabilities, NULL);
    else
        qmi_indication_nas_serving_system_output_get_data_service_capability (indication_output, &data_service_capabilities, NULL);

    if (data_service_capabilities)
        mm_access_technologies =
            qmi_data_capability_list_to_access_technologies (data_service_capabilities);
    else
        mm_access_technologies =
            qmi_radio_interface_list_to_access_technologies (radio_interfaces);

    /* Get and cache operator ID/name */
    if ((response_output &&
         qmi_message_nas_get_serving_system_output_get_current_plmn (
             response_output,
             &mcc,
             &mnc,
             &description,
             NULL)) ||
        (indication_output &&
         qmi_indication_nas_serving_system_output_get_current_plmn (
             indication_output,
             &mcc,
             &mnc,
             &description,
             NULL))) {
        /* When we don't have information about leading PCS digit, guess best */
        g_free (self->priv->current_operator_id);
        if (mnc >= 100)
            self->priv->current_operator_id =
                g_strdup_printf ("%.3" G_GUINT16_FORMAT "%.3" G_GUINT16_FORMAT,
                                 mcc,
                                 mnc);
        else
            self->priv->current_operator_id =
                g_strdup_printf ("%.3" G_GUINT16_FORMAT "%.2" G_GUINT16_FORMAT,
                                 mcc,
                                 mnc);

        g_free (self->priv->current_operator_description);
        self->priv->current_operator_description = g_strdup (description);
    }

    /* If MNC comes with PCS digit, we must make sure the additional
     * leading '0' is added */
    if (((response_output &&
          qmi_message_nas_get_serving_system_output_get_mnc_pcs_digit_include_status (
              response_output,
              &mcc,
              &mnc,
              &has_pcs_digit,
              NULL)) ||
         (indication_output &&
          qmi_indication_nas_serving_system_output_get_mnc_pcs_digit_include_status (
              indication_output,
              &mcc,
              &mnc,
              &has_pcs_digit,
              NULL))) &&
        has_pcs_digit) {
        g_free (self->priv->current_operator_id);
        self->priv->current_operator_id =
            g_strdup_printf ("%.3" G_GUINT16_FORMAT "%.3" G_GUINT16_FORMAT,
                             mcc,
                             mnc);
    }

    /* Get 3GPP location LAC and CI */
    lac = 0;
    cid = 0;
    if (response_output) {
        qmi_message_nas_get_serving_system_output_get_lac_3gpp (response_output, &lac, NULL);
        qmi_message_nas_get_serving_system_output_get_cid_3gpp (response_output, &cid, NULL);
    } else {
        qmi_indication_nas_serving_system_output_get_lac_3gpp (indication_output, &lac, NULL);
        qmi_indication_nas_serving_system_output_get_cid_3gpp (indication_output, &cid, NULL);
    }

    /* Report new registration states */
    mm_iface_modem_3gpp_update_cs_registration_state (MM_IFACE_MODEM_3GPP (self), mm_cs_registration_state);
    mm_iface_modem_3gpp_update_ps_registration_state (MM_IFACE_MODEM_3GPP (self), mm_ps_registration_state);
    mm_iface_modem_3gpp_update_access_technologies (MM_IFACE_MODEM_3GPP (self), mm_access_technologies);
    mm_iface_modem_3gpp_update_location (MM_IFACE_MODEM_3GPP (self), lac, cid);
}

static void
get_serving_system_3gpp_ready (QmiClientNas *client,
                               GAsyncResult *res,
                               Run3gppRegistrationChecksContext *ctx)
{
    QmiMessageNasGetServingSystemOutput *output;
    GError *error = NULL;

    output = qmi_client_nas_get_serving_system_finish (client, res, &error);
    if (!output) {
        g_prefix_error (&error, "QMI operation failed: ");
        g_simple_async_result_take_error (ctx->result, error);
        run_3gpp_registration_checks_context_complete_and_free (ctx);
        return;
    }

    if (!qmi_message_nas_get_serving_system_output_get_result (output, &error)) {
        g_prefix_error (&error, "Couldn't get serving system: ");
        g_simple_async_result_take_error (ctx->result, error);
        qmi_message_nas_get_serving_system_output_unref (output);
        run_3gpp_registration_checks_context_complete_and_free (ctx);
        return;
    }

    common_process_serving_system_3gpp (ctx->self, output, NULL);

    g_simple_async_result_set_op_res_gboolean (ctx->result, TRUE);
    qmi_message_nas_get_serving_system_output_unref (output);
    run_3gpp_registration_checks_context_complete_and_free (ctx);
}

static gboolean
process_common_info (QmiNasServiceStatus service_status,
                     gboolean domain_valid,
                     QmiNasNetworkServiceDomain domain,
                     gboolean roaming_status_valid,
                     QmiNasRoamingStatus roaming_status,
                     gboolean forbidden_valid,
                     gboolean forbidden,
                     gboolean lac_valid,
                     guint16 lac,
                     gboolean cid_valid,
                     guint32 cid,
                     gboolean network_id_valid,
                     const gchar *mcc,
                     const gchar *mnc,
                     MMModem3gppRegistrationState *mm_cs_registration_state,
                     MMModem3gppRegistrationState *mm_ps_registration_state,
                     guint16 *mm_lac,
                     guint32 *mm_cid,
                     gchar **mm_operator_id)
{
    MMModem3gppRegistrationState tmp_registration_state;
    gboolean apply_cs;
    gboolean apply_ps;

    if (service_status != QMI_NAS_SERVICE_STATUS_LIMITED &&
        service_status != QMI_NAS_SERVICE_STATUS_AVAILABLE &&
        service_status != QMI_NAS_SERVICE_STATUS_LIMITED_REGIONAL)
        return FALSE;

    /* If we don't have domain, unknown */
    if (!domain_valid)
        tmp_registration_state = MM_MODEM_3GPP_REGISTRATION_STATE_UNKNOWN;
    else if (domain == QMI_NAS_NETWORK_SERVICE_DOMAIN_NONE)
        tmp_registration_state = MM_MODEM_3GPP_REGISTRATION_STATE_SEARCHING;
    else if (domain == QMI_NAS_NETWORK_SERVICE_DOMAIN_UNKNOWN)
        tmp_registration_state = MM_MODEM_3GPP_REGISTRATION_STATE_UNKNOWN;
    else {
        /* If we have CS or PS service domain, assume registered for now */
        if (domain == QMI_NAS_NETWORK_SERVICE_DOMAIN_CS)
            apply_ps = FALSE;
        else if (domain == QMI_NAS_NETWORK_SERVICE_DOMAIN_PS)
            apply_cs = FALSE;

        /* Check if we really are roaming or forbidden */
        if (forbidden_valid && forbidden)
            tmp_registration_state = MM_MODEM_3GPP_REGISTRATION_STATE_DENIED;
        else {
            if (roaming_status_valid && roaming_status == QMI_NAS_ROAMING_STATUS_ON)
                tmp_registration_state = MM_MODEM_3GPP_REGISTRATION_STATE_ROAMING;
            else
                tmp_registration_state = MM_MODEM_3GPP_REGISTRATION_STATE_HOME;

            /* If we're registered either at home or roaming, try to get LAC/CID */
            if (lac_valid)
                *mm_lac = lac;
            if (cid_valid)
                *mm_cid = cid;
        }
    }

    if (apply_cs)
        *mm_cs_registration_state = tmp_registration_state;
    if (apply_ps)
        *mm_cs_registration_state = tmp_registration_state;

    if (network_id_valid) {
        *mm_operator_id = g_malloc (7);
        memcpy (*mm_operator_id, mcc, 3);
        if (mnc[2] == 0xFF) {
            memcpy (*mm_operator_id, mnc, 2);
            (*mm_operator_id)[5] = '\0';
        } else {
            memcpy (*mm_operator_id, mnc, 3);
            (*mm_operator_id)[6] = '\0';
        }
    }

    return TRUE;
}

static gboolean
process_gsm_info (QmiMessageNasGetSystemInfoOutput *response_output,
                  QmiIndicationNasSystemInfoOutput *indication_output,
                  MMModemAccessTechnology *mm_access_technologies,
                  MMModem3gppRegistrationState *mm_cs_registration_state,
                  MMModem3gppRegistrationState *mm_ps_registration_state,
                  guint16 *mm_lac,
                  guint32 *mm_cid,
                  gchar **mm_operator_id)
{
    QmiNasServiceStatus service_status;
    gboolean domain_valid;
    QmiNasNetworkServiceDomain domain;
    gboolean roaming_status_valid;
    QmiNasRoamingStatus roaming_status;
    gboolean forbidden_valid;
    gboolean forbidden;
    gboolean lac_valid;
    guint16 lac;
    gboolean cid_valid;
    guint32 cid;
    gboolean network_id_valid;
    const gchar *mcc;
    const gchar *mnc;
    gboolean egprs_support_valid;
    gboolean egprs_support;

    g_assert ((response_output != NULL && indication_output == NULL) ||
              (response_output == NULL && indication_output != NULL));

    *mm_ps_registration_state = MM_MODEM_3GPP_REGISTRATION_STATE_UNKNOWN;
    *mm_cs_registration_state = MM_MODEM_3GPP_REGISTRATION_STATE_UNKNOWN;
    *mm_lac = 0;
    *mm_cid = 0;
    g_free (*mm_operator_id);
    *mm_operator_id = NULL;

    if (response_output) {
        if (!qmi_message_nas_get_system_info_output_get_gsm_service_status (
                response_output,
                &service_status,
                NULL, /* true_service_status */
                NULL, /* preferred_data_path */
                NULL) ||
            !qmi_message_nas_get_system_info_output_get_gsm_system_info (
                response_output,
                &domain_valid,         &domain,
                NULL, NULL, /* service_capability */
                &roaming_status_valid, &roaming_status,
                &forbidden_valid,      &forbidden,
                &lac_valid,            &lac,
                &cid_valid,            &cid,
                NULL, NULL, NULL, /* registration_reject_info */
                &network_id_valid,     &mcc, &mnc,
                &egprs_support_valid,  &egprs_support,
                NULL, NULL, /* dtm_support */
                NULL)) {
            mm_dbg ("No GSM service reported");
            /* No GSM service */
            return FALSE;
        }
    } else {
        if (!qmi_indication_nas_system_info_output_get_gsm_service_status (
                indication_output,
                &service_status,
                NULL, /* true_service_status */
                NULL, /* preferred_data_path */
                NULL) ||
            !qmi_indication_nas_system_info_output_get_gsm_system_info (
                indication_output,
                &domain_valid,         &domain,
                NULL, NULL, /* service_capability */
                &roaming_status_valid, &roaming_status,
                &forbidden_valid,      &forbidden,
                &lac_valid,            &lac,
                &cid_valid,            &cid,
                NULL, NULL, NULL, /* registration_reject_info */
                &network_id_valid,     &mcc, &mnc,
                &egprs_support_valid,  &egprs_support,
                NULL, NULL, /* dtm_support */
                NULL)) {
            mm_dbg ("No GSM service reported");
            /* No GSM service */
            return FALSE;
        }
    }

    if (!process_common_info (service_status,
                              domain_valid,         domain,
                              roaming_status_valid, roaming_status,
                              forbidden_valid,      forbidden,
                              lac_valid,            lac,
                              cid_valid,            cid,
                              network_id_valid,     mcc, mnc,
                              mm_cs_registration_state,
                              mm_ps_registration_state,
                              mm_lac,
                              mm_cid,
                              mm_operator_id)) {
        mm_dbg ("No GSM service registered");
        return FALSE;
    }

    *mm_access_technologies |= MM_MODEM_ACCESS_TECHNOLOGY_GSM;
    if (egprs_support_valid && egprs_support)
        *mm_access_technologies |= MM_MODEM_ACCESS_TECHNOLOGY_EDGE;

    return TRUE;
}

static gboolean
process_wcdma_info (QmiMessageNasGetSystemInfoOutput *response_output,
                    QmiIndicationNasSystemInfoOutput *indication_output,
                    MMModemAccessTechnology *mm_access_technologies,
                    MMModem3gppRegistrationState *mm_cs_registration_state,
                    MMModem3gppRegistrationState *mm_ps_registration_state,
                    guint16 *mm_lac,
                    guint32 *mm_cid,
                    gchar **mm_operator_id)
{
    QmiNasServiceStatus service_status;
    gboolean domain_valid;
    QmiNasNetworkServiceDomain domain;
    gboolean roaming_status_valid;
    QmiNasRoamingStatus roaming_status;
    gboolean forbidden_valid;
    gboolean forbidden;
    gboolean lac_valid;
    guint16 lac;
    gboolean cid_valid;
    guint32 cid;
    gboolean network_id_valid;
    const gchar *mcc;
    const gchar *mnc;
    gboolean hs_service_valid;
    QmiNasWcdmaHsService hs_service;

    g_assert ((response_output != NULL && indication_output == NULL) ||
              (response_output == NULL && indication_output != NULL));

    *mm_ps_registration_state = MM_MODEM_3GPP_REGISTRATION_STATE_UNKNOWN;
    *mm_cs_registration_state = MM_MODEM_3GPP_REGISTRATION_STATE_UNKNOWN;
    *mm_lac = 0;
    *mm_cid = 0;
    g_free (*mm_operator_id);
    *mm_operator_id = NULL;

    if (response_output) {
        if (!qmi_message_nas_get_system_info_output_get_wcdma_service_status (
                response_output,
                &service_status,
                NULL, /* true_service_status */
                NULL, /* preferred_data_path */
                NULL) ||
            !qmi_message_nas_get_system_info_output_get_wcdma_system_info (
                response_output,
                &domain_valid,         &domain,
                NULL, NULL, /* service_capability */
                &roaming_status_valid, &roaming_status,
                &forbidden_valid,      &forbidden,
                &lac_valid,            &lac,
                &cid_valid,            &cid,
                NULL, NULL, NULL, /* registration_reject_info */
                &network_id_valid,     &mcc, &mnc,
                NULL, NULL, /* hs_call_status */
                &hs_service_valid,     &hs_service,
                NULL, NULL, /* primary_scrambling_code */
                NULL)) {
            mm_dbg ("No WCDMA service reported");
            /* No GSM service */
            return FALSE;
        }
    } else {
        if (!qmi_indication_nas_system_info_output_get_wcdma_service_status (
                indication_output,
                &service_status,
                NULL, /* true_service_status */
                NULL, /* preferred_data_path */
                NULL) ||
            !qmi_indication_nas_system_info_output_get_wcdma_system_info (
                indication_output,
                &domain_valid,         &domain,
                NULL, NULL, /* service_capability */
                &roaming_status_valid, &roaming_status,
                &forbidden_valid,      &forbidden,
                &lac_valid,            &lac,
                &cid_valid,            &cid,
                NULL, NULL, NULL, /* registration_reject_info */
                &network_id_valid,     &mcc, &mnc,
                NULL, NULL, /* hs_call_status */
                &hs_service_valid,     &hs_service,
                NULL, NULL, /* primary_scrambling_code */
                NULL)) {
            mm_dbg ("No WCDMA service reported");
            /* No GSM service */
            return FALSE;
        }
    }

    if (!process_common_info (service_status,
                              domain_valid,         domain,
                              roaming_status_valid, roaming_status,
                              forbidden_valid,      forbidden,
                              lac_valid,            lac,
                              cid_valid,            cid,
                              network_id_valid,     mcc, mnc,
                              mm_cs_registration_state,
                              mm_ps_registration_state,
                              mm_lac,
                              mm_cid,
                              mm_operator_id)) {
        mm_dbg ("No WCDMA service registered");
        return FALSE;
    }

    *mm_access_technologies |= MM_MODEM_ACCESS_TECHNOLOGY_UMTS;
    if (hs_service_valid) {
        switch (hs_service) {
        case QMI_NAS_WCDMA_HS_SERVICE_HSDPA_HSUPA_UNSUPPORTED:
            break;
        case QMI_NAS_WCDMA_HS_SERVICE_HSDPA_SUPPORTED:
            *mm_access_technologies |= MM_MODEM_ACCESS_TECHNOLOGY_HSDPA;
            break;
        case QMI_NAS_WCDMA_HS_SERVICE_HSUPA_SUPPORTED:
            *mm_access_technologies |= MM_MODEM_ACCESS_TECHNOLOGY_HSUPA;
            break;
        case QMI_NAS_WCDMA_HS_SERVICE_HSDPA_HSUPA_SUPPORTED:
            *mm_access_technologies |= MM_MODEM_ACCESS_TECHNOLOGY_HSPA;
            break;
        case QMI_NAS_WCDMA_HS_SERVICE_HSDPA_PLUS_SUPPORTED:
        case QMI_NAS_WCDMA_HS_SERVICE_HSDPA_PLUS_HSUPA_SUPPORTED:
        case QMI_NAS_WCDMA_HS_SERVICE_DC_HSDPA_PLUS_SUPPORTED:
        case QMI_NAS_WCDMA_HS_SERVICE_DC_HSDPA_PLUS_HSUPA_SUPPORTED:
            *mm_access_technologies |= MM_MODEM_ACCESS_TECHNOLOGY_HSPA_PLUS;
            break;
        }
    }

    return TRUE;
}

static gboolean
process_lte_info (QmiMessageNasGetSystemInfoOutput *response_output,
                  QmiIndicationNasSystemInfoOutput *indication_output,
                  MMModemAccessTechnology *mm_access_technologies,
                  MMModem3gppRegistrationState *mm_cs_registration_state,
                  MMModem3gppRegistrationState *mm_ps_registration_state,
                  guint16 *mm_lac,
                  guint32 *mm_cid,
                  gchar **mm_operator_id)
{
    QmiNasServiceStatus service_status;
    gboolean domain_valid;
    QmiNasNetworkServiceDomain domain;
    gboolean roaming_status_valid;
    QmiNasRoamingStatus roaming_status;
    gboolean forbidden_valid;
    gboolean forbidden;
    gboolean lac_valid;
    guint16 lac;
    gboolean cid_valid;
    guint32 cid;
    gboolean network_id_valid;
    const gchar *mcc;
    const gchar *mnc;

    g_assert ((response_output != NULL && indication_output == NULL) ||
              (response_output == NULL && indication_output != NULL));

    *mm_ps_registration_state = MM_MODEM_3GPP_REGISTRATION_STATE_UNKNOWN;
    *mm_cs_registration_state = MM_MODEM_3GPP_REGISTRATION_STATE_UNKNOWN;
    *mm_lac = 0;
    *mm_cid = 0;
    g_free (*mm_operator_id);
    *mm_operator_id = NULL;

    if (response_output) {
        if (!qmi_message_nas_get_system_info_output_get_lte_service_status (
                response_output,
                &service_status,
                NULL, /* true_service_status */
                NULL, /* preferred_data_path */
                NULL) ||
            !qmi_message_nas_get_system_info_output_get_lte_system_info (
                response_output,
                &domain_valid,         &domain,
                NULL, NULL, /* service_capability */
                &roaming_status_valid, &roaming_status,
                &forbidden_valid,      &forbidden,
                &lac_valid,            &lac,
                &cid_valid,            &cid,
                NULL, NULL, NULL, /* registration_reject_info */
                &network_id_valid,     &mcc, &mnc,
                NULL, NULL, /* tac */
                NULL)) {
            mm_dbg ("No LTE service reported");
            /* No GSM service */
            return FALSE;
        }
    } else {
        if (!qmi_indication_nas_system_info_output_get_lte_service_status (
                indication_output,
                &service_status,
                NULL, /* true_service_status */
                NULL, /* preferred_data_path */
                NULL) ||
            !qmi_indication_nas_system_info_output_get_lte_system_info (
                indication_output,
                &domain_valid,         &domain,
                NULL, NULL, /* service_capability */
                &roaming_status_valid, &roaming_status,
                &forbidden_valid,      &forbidden,
                &lac_valid,            &lac,
                &cid_valid,            &cid,
                NULL, NULL, NULL, /* registration_reject_info */
                &network_id_valid,     &mcc, &mnc,
                NULL, NULL, /* tac */
                NULL)) {
            mm_dbg ("No LTE service reported");
            /* No GSM service */
            return FALSE;
        }
    }

    if (!process_common_info (service_status,
                              domain_valid,         domain,
                              roaming_status_valid, roaming_status,
                              forbidden_valid,      forbidden,
                              lac_valid,            lac,
                              cid_valid,            cid,
                              network_id_valid,     mcc, mnc,
                              mm_cs_registration_state,
                              mm_ps_registration_state,
                              mm_lac,
                              mm_cid,
                              mm_operator_id)) {
        mm_dbg ("No LTE service registered");
        return FALSE;
    }

    *mm_access_technologies |= MM_MODEM_ACCESS_TECHNOLOGY_LTE;
    return TRUE;
}

static void
common_process_system_info_3gpp (MMBroadbandModemQmi *self,
                                 QmiMessageNasGetSystemInfoOutput *response_output,
                                 QmiIndicationNasSystemInfoOutput *indication_output)
{
    MMModemAccessTechnology access_technologies;
    MMModem3gppRegistrationState cs_registration_state;
    MMModem3gppRegistrationState ps_registration_state;
    guint16 lac;
    guint32 cid;
    gchar *operator_id;

    access_technologies = 0;
    ps_registration_state = MM_MODEM_3GPP_REGISTRATION_STATE_UNKNOWN;
    cs_registration_state = MM_MODEM_3GPP_REGISTRATION_STATE_UNKNOWN;
    lac = 0;
    cid = 0;
    operator_id = NULL;

    /* Process infos, with the following priority:
     *   LTE > WCDMA > GSM
     * The first one giving results will be the one reported.
     */
    if (!process_lte_info (response_output, indication_output,
                           &access_technologies,
                           &cs_registration_state,
                           &ps_registration_state,
                           &lac,
                           &cid,
                           &operator_id) &&
        !process_wcdma_info (response_output, indication_output,
                             &access_technologies,
                             &cs_registration_state,
                             &ps_registration_state,
                             &lac,
                             &cid,
                             &operator_id) &&
        !process_gsm_info (response_output, indication_output,
                           &access_technologies,
                           &cs_registration_state,
                           &ps_registration_state,
                           &lac,
                           &cid,
                           &operator_id)) {
        mm_dbg ("No service (GSM, WCDMA or LTE) reported");
    }

    /* Cache current operator ID */
    if (operator_id) {
        g_free (self->priv->current_operator_id);
        self->priv->current_operator_id = operator_id;
    }

    /* Report new registration states */
    mm_iface_modem_3gpp_update_cs_registration_state (MM_IFACE_MODEM_3GPP (self), cs_registration_state);
    mm_iface_modem_3gpp_update_ps_registration_state (MM_IFACE_MODEM_3GPP (self), ps_registration_state);
    mm_iface_modem_3gpp_update_access_technologies (MM_IFACE_MODEM_3GPP (self), access_technologies);
    mm_iface_modem_3gpp_update_location (MM_IFACE_MODEM_3GPP (self), lac, cid);
}

static void
get_system_info_ready (QmiClientNas *client,
                       GAsyncResult *res,
                       Run3gppRegistrationChecksContext *ctx)
{
    QmiMessageNasGetSystemInfoOutput *output;
    GError *error = NULL;

    output = qmi_client_nas_get_system_info_finish (client, res, &error);
    if (!output) {
        g_prefix_error (&error, "QMI operation failed: ");
        g_simple_async_result_take_error (ctx->result, error);
        run_3gpp_registration_checks_context_complete_and_free (ctx);
        return;
    }

    if (!qmi_message_nas_get_system_info_output_get_result (output, &error)) {
        g_prefix_error (&error, "Couldn't get system info: ");
        g_simple_async_result_take_error (ctx->result, error);
        qmi_message_nas_get_system_info_output_unref (output);
        run_3gpp_registration_checks_context_complete_and_free (ctx);
        return;
    }

    common_process_system_info_3gpp (ctx->self, output, NULL);

    g_simple_async_result_set_op_res_gboolean (ctx->result, TRUE);
    qmi_message_nas_get_system_info_output_unref (output);
    run_3gpp_registration_checks_context_complete_and_free (ctx);
}

static void
modem_3gpp_run_registration_checks (MMIfaceModem3gpp *self,
                                    gboolean cs_supported,
                                    gboolean ps_supported,
                                    GAsyncReadyCallback callback,
                                    gpointer user_data)
{
    Run3gppRegistrationChecksContext *ctx;
    QmiClient *client = NULL;

    if (!ensure_qmi_client (MM_BROADBAND_MODEM_QMI (self),
                            QMI_SERVICE_NAS, &client,
                            callback, user_data))
        return;

    ctx = g_new0 (Run3gppRegistrationChecksContext, 1);
    ctx->self = g_object_ref (self);
    ctx->client = g_object_ref (client);
    ctx->result = g_simple_async_result_new (G_OBJECT (self),
                                             callback,
                                             user_data,
                                             modem_3gpp_run_registration_checks);

    /* System Info was added in NAS 1.8 */
    if (qmi_client_check_version (client, 1, 8))
        qmi_client_nas_get_system_info (ctx->client,
                                        NULL,
                                        10,
                                        NULL,
                                        (GAsyncReadyCallback)get_system_info_ready,
                                        ctx);
    else
        qmi_client_nas_get_serving_system (ctx->client,
                                           NULL,
                                           10,
                                           NULL,
                                           (GAsyncReadyCallback)get_serving_system_3gpp_ready,
                                           ctx);
}

/*****************************************************************************/
/* Enable/Disable unsolicited registration events (3GPP interface) */

typedef struct {
    MMBroadbandModemQmi *self;
    QmiClientNas *client;
    GSimpleAsyncResult *result;
    gboolean enable; /* TRUE for enabling, FALSE for disabling */
} UnsolicitedRegistrationEventsContext;

static void
unsolicited_registration_events_context_complete_and_free (UnsolicitedRegistrationEventsContext *ctx)
{
    g_simple_async_result_complete_in_idle (ctx->result);
    g_object_unref (ctx->result);
    g_object_unref (ctx->client);
    g_object_unref (ctx->self);
    g_free (ctx);
}

static UnsolicitedRegistrationEventsContext *
unsolicited_registration_events_context_new (MMBroadbandModemQmi *self,
                                             QmiClient *client,
                                             gboolean enable,
                                             GAsyncReadyCallback callback,
                                             gpointer user_data)
{
    UnsolicitedRegistrationEventsContext *ctx;

    ctx = g_new0 (UnsolicitedRegistrationEventsContext, 1);
    ctx->self = g_object_ref (self);
    ctx->client = g_object_ref (client);
    ctx->result = g_simple_async_result_new (G_OBJECT (self),
                                             callback,
                                             user_data,
                                             unsolicited_registration_events_context_new);
    ctx->enable = FALSE;
    return ctx;
}

static gboolean
modem_3gpp_enable_disable_unsolicited_registration_events_finish (MMIfaceModem3gpp *self,
                                                                  GAsyncResult *res,
                                                                  GError **error)
{
    return !g_simple_async_result_propagate_error (G_SIMPLE_ASYNC_RESULT (res), error);
}

static void
ri_serving_system_or_system_info_ready (QmiClientNas *client,
                                        GAsyncResult *res,
                                        UnsolicitedRegistrationEventsContext *ctx)
{
    QmiMessageNasRegisterIndicationsOutput *output = NULL;
    GError *error = NULL;

    output = qmi_client_nas_register_indications_finish (client, res, &error);
    if (!output) {
        mm_dbg ("QMI operation failed: '%s'", error->message);
        g_error_free (error);
    } else if (!qmi_message_nas_register_indications_output_get_result (output, &error)) {
        mm_dbg ("Couldn't register indications: '%s'", error->message);
        g_error_free (error);
    }

    if (output)
        qmi_message_nas_register_indications_output_unref (output);

    /* Just ignore errors for now */
    ctx->self->priv->unsolicited_registration_events_enabled = ctx->enable;
    g_simple_async_result_set_op_res_gboolean (ctx->result, TRUE);
    unsolicited_registration_events_context_complete_and_free (ctx);
}

static void
common_enable_disable_unsolicited_registration_events_serving_system (UnsolicitedRegistrationEventsContext *ctx)
{
    QmiMessageNasRegisterIndicationsInput *input;

    input = qmi_message_nas_register_indications_input_new ();
    qmi_message_nas_register_indications_input_set_serving_system_events (input, ctx->enable, NULL);
    qmi_client_nas_register_indications (
        ctx->client,
        input,
        5,
        NULL,
        (GAsyncReadyCallback)ri_serving_system_or_system_info_ready,
        ctx);
    qmi_message_nas_register_indications_input_unref (input);
}

static void
common_enable_disable_unsolicited_registration_events_system_info (UnsolicitedRegistrationEventsContext *ctx)
{
    QmiMessageNasRegisterIndicationsInput *input;

    input = qmi_message_nas_register_indications_input_new ();
    qmi_message_nas_register_indications_input_set_system_info (input, ctx->enable, NULL);
    qmi_client_nas_register_indications (
        ctx->client,
        input,
        5,
        NULL,
        (GAsyncReadyCallback)ri_serving_system_or_system_info_ready,
        ctx);
    qmi_message_nas_register_indications_input_unref (input);
}

static void
modem_3gpp_disable_unsolicited_registration_events (MMIfaceModem3gpp *self,
                                                    gboolean cs_supported,
                                                    gboolean ps_supported,
                                                    GAsyncReadyCallback callback,
                                                    gpointer user_data)
{
    UnsolicitedRegistrationEventsContext *ctx;
    QmiClient *client = NULL;

    if (!ensure_qmi_client (MM_BROADBAND_MODEM_QMI (self),
                            QMI_SERVICE_NAS, &client,
                            callback, user_data))
        return;

    ctx = unsolicited_registration_events_context_new (MM_BROADBAND_MODEM_QMI (self),
                                                       client,
                                                       FALSE,
                                                       callback,
                                                       user_data);

    /* System Info was added in NAS 1.8 */
    if (qmi_client_check_version (client, 1, 8)) {
        common_enable_disable_unsolicited_registration_events_system_info (ctx);
        return;
    }

    /* Ability to explicitly enable/disable serving system indications was
     * added in NAS 1.2 */
    if (qmi_client_check_version (client, 1, 2)) {
        common_enable_disable_unsolicited_registration_events_serving_system (ctx);
        return;
    }

    /* Devices with NAS < 1.2 will just always issue serving system indications */
    g_simple_async_result_set_error (ctx->result,
                                     MM_CORE_ERROR,
                                     MM_CORE_ERROR_FAILED,
                                     "Device doesn't allow disabling registration events");
    ctx->self->priv->unsolicited_registration_events_enabled = FALSE;
    unsolicited_registration_events_context_complete_and_free (ctx);
}

static void
modem_3gpp_enable_unsolicited_registration_events (MMIfaceModem3gpp *self,
                                                   gboolean cs_supported,
                                                   gboolean ps_supported,
                                                   GAsyncReadyCallback callback,
                                                   gpointer user_data)
{
    UnsolicitedRegistrationEventsContext *ctx;
    QmiClient *client = NULL;

    if (!ensure_qmi_client (MM_BROADBAND_MODEM_QMI (self),
                            QMI_SERVICE_NAS, &client,
                            callback, user_data))
        return;

    ctx = unsolicited_registration_events_context_new (MM_BROADBAND_MODEM_QMI (self),
                                                       client,
                                                       TRUE,
                                                       callback,
                                                       user_data);

    /* Ability to explicitly enable/disable serving system indications was
     * added in NAS 1.2 */
    if (qmi_client_check_version (client, 1, 2)) {
        common_enable_disable_unsolicited_registration_events_serving_system (ctx);
        return;
    }

    /* Devices with NAS < 1.2 will just always issue serving system indications */
    mm_dbg ("Assuming serving system indications are always enabled");
    g_simple_async_result_set_op_res_gboolean (ctx->result, TRUE);
    ctx->self->priv->unsolicited_registration_events_enabled = TRUE;
    unsolicited_registration_events_context_complete_and_free (ctx);
}

/*****************************************************************************/
/* Registration checks (CDMA interface) */

typedef struct {
    MMBroadbandModemQmi *self;
    QmiClientNas *client;
    GSimpleAsyncResult *result;
} RunCdmaRegistrationChecksContext;

static void
run_cdma_registration_checks_context_complete_and_free (RunCdmaRegistrationChecksContext *ctx)
{
    g_simple_async_result_complete (ctx->result);
    g_object_unref (ctx->result);
    g_object_unref (ctx->client);
    g_object_unref (ctx->self);
    g_slice_free (RunCdmaRegistrationChecksContext, ctx);
}

static gboolean
modem_cdma_run_registration_checks_finish (MMIfaceModemCdma *self,
                                           GAsyncResult *res,
                                           GError **error)
{
    return !g_simple_async_result_propagate_error (G_SIMPLE_ASYNC_RESULT (res), error);
}

static MMModemCdmaRegistrationState
qmi_registration_state_to_cdma_registration_state (QmiNasRegistrationState registration_state)
{
    switch (registration_state) {
    case QMI_NAS_REGISTRATION_STATE_REGISTERED:
        return MM_MODEM_CDMA_REGISTRATION_STATE_REGISTERED;
    case QMI_NAS_REGISTRATION_STATE_NOT_REGISTERED:
    case QMI_NAS_REGISTRATION_STATE_NOT_REGISTERED_SEARCHING:
    case QMI_NAS_REGISTRATION_STATE_REGISTRATION_DENIED:
    case QMI_NAS_REGISTRATION_STATE_UNKNOWN:
    default:
        return MM_MODEM_3GPP_REGISTRATION_STATE_UNKNOWN;
    }
}

static void
common_process_serving_system_cdma (MMBroadbandModemQmi *self,
                                    QmiMessageNasGetServingSystemOutput *response_output,
                                    QmiIndicationNasServingSystemOutput *indication_output)
{
    QmiNasRegistrationState registration_state;
    QmiNasNetworkType selected_network;
    GArray *radio_interfaces;
    GArray *data_service_capabilities;
    QmiNasServiceStatus hdr_status;
    gboolean hdr_hybrid;
    MMModemAccessTechnology mm_access_technologies;
    MMModemCdmaRegistrationState mm_cdma1x_registration_state;
    MMModemCdmaRegistrationState mm_evdo_registration_state;
    guint16 sid;
    guint16 nid;

    if (response_output)
        qmi_message_nas_get_serving_system_output_get_serving_system (
            response_output,
            &registration_state,
            NULL, /* cs_attach_state */
            NULL, /* ps_attach_state */
            &selected_network,
            &radio_interfaces,
            NULL);
    else
        qmi_indication_nas_serving_system_output_get_serving_system (
            indication_output,
            &registration_state,
            NULL, /* cs_attach_state */
            NULL, /* ps_attach_state */
            &selected_network,
            &radio_interfaces,
            NULL);

    /* Only process 3GPP2 info */
    if (selected_network != QMI_NAS_NETWORK_TYPE_3GPP2) {
        mm_iface_modem_cdma_update_cdma1x_registration_state (MM_IFACE_MODEM_CDMA (self),
                                                              MM_MODEM_CDMA_REGISTRATION_STATE_UNKNOWN,
                                                              0, 0);
        mm_iface_modem_cdma_update_evdo_registration_state (MM_IFACE_MODEM_CDMA (self),
                                                             MM_MODEM_CDMA_REGISTRATION_STATE_UNKNOWN);
        mm_iface_modem_cdma_update_access_technologies (MM_IFACE_MODEM_CDMA (self),
                                                        MM_MODEM_ACCESS_TECHNOLOGY_UNKNOWN);
        return;
    }

    /* Get SID/NID */
    if (response_output)
        qmi_message_nas_get_serving_system_output_get_cdma_system_id (response_output, &sid, &nid, NULL);
    else
        qmi_indication_nas_serving_system_output_get_cdma_system_id (indication_output, &sid, &nid, NULL);

    /* Build access technologies mask */
    data_service_capabilities = NULL;
    if (response_output)
        qmi_message_nas_get_serving_system_output_get_data_service_capability (response_output,
                                                                               &data_service_capabilities,
                                                                               NULL);
    else
        qmi_indication_nas_serving_system_output_get_data_service_capability (indication_output,
                                                                              &data_service_capabilities,
                                                                              NULL);
    if (data_service_capabilities)
        mm_access_technologies =
            qmi_data_capability_list_to_access_technologies (data_service_capabilities);
    else
        mm_access_technologies =
            qmi_radio_interface_list_to_access_technologies (radio_interfaces);

    /* TODO: Roaming flags */

    /* Build registration states */
    mm_cdma1x_registration_state = qmi_registration_state_to_cdma_registration_state (registration_state);

    hdr_status = QMI_NAS_SERVICE_STATUS_NONE;
    hdr_hybrid = FALSE;
    if (response_output)
        qmi_message_nas_get_serving_system_output_get_detailed_service_status (
            response_output,
            NULL, /* status */
            NULL, /* capability */
            &hdr_status,
            &hdr_hybrid,
            NULL, /* forbidden */
            NULL);
    else
        qmi_indication_nas_serving_system_output_get_detailed_service_status (
            indication_output,
            NULL, /* status */
            NULL, /* capability */
            &hdr_status,
            &hdr_hybrid,
            NULL, /* forbidden */
            NULL);

    if (hdr_hybrid &&
        (hdr_status == QMI_NAS_SERVICE_STATUS_LIMITED ||
         hdr_status == QMI_NAS_SERVICE_STATUS_AVAILABLE ||
         hdr_status == QMI_NAS_SERVICE_STATUS_LIMITED_REGIONAL ||
         hdr_status == QMI_NAS_SERVICE_STATUS_POWER_SAVE))
        mm_evdo_registration_state = MM_MODEM_CDMA_REGISTRATION_STATE_REGISTERED;
    else
        mm_evdo_registration_state = MM_MODEM_CDMA_REGISTRATION_STATE_UNKNOWN;

    /* Report new registration states */
    mm_iface_modem_cdma_update_cdma1x_registration_state (MM_IFACE_MODEM_CDMA (self),
                                                          mm_cdma1x_registration_state,
                                                          sid,
                                                          nid);
    mm_iface_modem_cdma_update_evdo_registration_state (MM_IFACE_MODEM_CDMA (self),
                                                        mm_evdo_registration_state);
    mm_iface_modem_cdma_update_access_technologies (MM_IFACE_MODEM_CDMA (self),
                                                    mm_access_technologies);
}

static void
get_serving_system_cdma_ready (QmiClientNas *client,
                               GAsyncResult *res,
                               RunCdmaRegistrationChecksContext *ctx)
{
    QmiMessageNasGetServingSystemOutput *output;
    GError *error = NULL;

    output = qmi_client_nas_get_serving_system_finish (client, res, &error);
    if (!output) {
        g_prefix_error (&error, "QMI operation failed: ");
        g_simple_async_result_take_error (ctx->result, error);
        run_cdma_registration_checks_context_complete_and_free (ctx);
        return;
    }

    if (!qmi_message_nas_get_serving_system_output_get_result (output, &error)) {
        g_prefix_error (&error, "Couldn't get serving system: ");
        g_simple_async_result_take_error (ctx->result, error);
        qmi_message_nas_get_serving_system_output_unref (output);
        run_cdma_registration_checks_context_complete_and_free (ctx);
        return;
    }

    common_process_serving_system_cdma (ctx->self, output, NULL);

    qmi_message_nas_get_serving_system_output_unref (output);
    g_simple_async_result_set_op_res_gboolean (ctx->result, TRUE);
    run_cdma_registration_checks_context_complete_and_free (ctx);
}

static void
modem_cdma_run_registration_checks (MMIfaceModemCdma *self,
                                    gboolean cdma1x_supported,
                                    gboolean evdo_supported,
                                    GAsyncReadyCallback callback,
                                    gpointer user_data)
{
    RunCdmaRegistrationChecksContext *ctx;
    QmiClient *client = NULL;

    if (!ensure_qmi_client (MM_BROADBAND_MODEM_QMI (self),
                            QMI_SERVICE_NAS, &client,
                            callback, user_data))
        return;

    /* Setup context */
    ctx = g_slice_new0 (RunCdmaRegistrationChecksContext);
    ctx->self = g_object_ref (self);
    ctx->client = g_object_ref (client);
    ctx->result = g_simple_async_result_new (G_OBJECT (self),
                                             callback,
                                             user_data,
                                             modem_cdma_run_registration_checks);

    /* TODO: Run Get System Info in NAS >= 1.8 */

    qmi_client_nas_get_serving_system (ctx->client,
                                       NULL,
                                       10,
                                       NULL,
                                       (GAsyncReadyCallback)get_serving_system_cdma_ready,
                                       ctx);
}

/*****************************************************************************/
/* Setup/Cleanup unsolicited registration event handlers
 * (3GPP and CDMA interface) */

static gboolean
common_setup_cleanup_unsolicited_registration_events_finish (MMBroadbandModemQmi *self,
                                                             GAsyncResult *res,
                                                             GError **error)
{
    return !g_simple_async_result_propagate_error (G_SIMPLE_ASYNC_RESULT (res), error);
}

static void
system_info_indication_cb (QmiClientNas *client,
                           QmiIndicationNasSystemInfoOutput *output,
                           MMBroadbandModemQmi *self)
{
    if (mm_iface_modem_is_3gpp (MM_IFACE_MODEM (self)))
        common_process_system_info_3gpp (self, NULL, output);
}

static void
serving_system_indication_cb (QmiClientNas *client,
                              QmiIndicationNasServingSystemOutput *output,
                              MMBroadbandModemQmi *self)
{
    if (mm_iface_modem_is_3gpp (MM_IFACE_MODEM (self)))
        common_process_serving_system_3gpp (self, NULL, output);
    else if (mm_iface_modem_is_cdma (MM_IFACE_MODEM (self)))
        common_process_serving_system_cdma (self, NULL, output);
}

static void
common_setup_cleanup_unsolicited_registration_events (MMBroadbandModemQmi *self,
                                                      gboolean enable,
                                                      GAsyncReadyCallback callback,
                                                      gpointer user_data)
{
    GSimpleAsyncResult *result;
    QmiClient *client = NULL;

    if (!ensure_qmi_client (MM_BROADBAND_MODEM_QMI (self),
                            QMI_SERVICE_NAS, &client,
                            callback, user_data))
        return;

    result = g_simple_async_result_new (G_OBJECT (self),
                                        callback,
                                        user_data,
                                        common_setup_cleanup_unsolicited_registration_events);

    if (enable == self->priv->unsolicited_registration_events_setup) {
        mm_dbg ("Unsolicited registration events already %s; skipping",
                enable ? "setup" : "cleanup");
        g_simple_async_result_set_op_res_gboolean (result, TRUE);
        g_simple_async_result_complete_in_idle (result);
        g_object_unref (result);
        return;
    }

    /* Store new state */
    self->priv->unsolicited_registration_events_setup = enable;

    /* Signal info introduced in NAS 1.8 */
    if (qmi_client_check_version (client, 1, 8)) {
        /* Connect/Disconnect "System Info" indications */
        if (enable) {
            g_assert (self->priv->system_info_indication_id == 0);
            self->priv->system_info_indication_id =
                g_signal_connect (client,
                                  "system-info",
                                  G_CALLBACK (system_info_indication_cb),
                                  self);
        } else {
            g_assert (self->priv->system_info_indication_id != 0);
            g_signal_handler_disconnect (client, self->priv->system_info_indication_id);
            self->priv->system_info_indication_id = 0;
        }
    } else {
        /* Connect/Disconnect "Serving System" indications */
        if (enable) {
            g_assert (self->priv->serving_system_indication_id == 0);
            self->priv->serving_system_indication_id =
                g_signal_connect (client,
                                  "serving-system",
                                  G_CALLBACK (serving_system_indication_cb),
                                  self);
        } else {
            g_assert (self->priv->serving_system_indication_id != 0);
            g_signal_handler_disconnect (client, self->priv->serving_system_indication_id);
            self->priv->serving_system_indication_id = 0;
        }
    }

    g_simple_async_result_set_op_res_gboolean (result, TRUE);
    g_simple_async_result_complete_in_idle (result);
    g_object_unref (result);
}

/*****************************************************************************/
/* Setup/Cleanup unsolicited registration events (3GPP interface) */

static gboolean
modem_3gpp_setup_cleanup_unsolicited_registration_events_finish (MMIfaceModem3gpp *self,
                                                                 GAsyncResult *res,
                                                                 GError **error)

{    return common_setup_cleanup_unsolicited_registration_events_finish (MM_BROADBAND_MODEM_QMI (self), res, error);
}

static void
modem_3gpp_cleanup_unsolicited_registration_events (MMIfaceModem3gpp *self,
                                                    GAsyncReadyCallback callback,
                                                    gpointer user_data)
{
    common_setup_cleanup_unsolicited_registration_events (MM_BROADBAND_MODEM_QMI (self),
                                                          FALSE,
                                                          callback,
                                                          user_data);
}

static void
modem_3gpp_setup_unsolicited_registration_events (MMIfaceModem3gpp *self,
                                                  GAsyncReadyCallback callback,
                                                  gpointer user_data)
{
    common_setup_cleanup_unsolicited_registration_events (MM_BROADBAND_MODEM_QMI (self),
                                                          TRUE,
                                                          callback,
                                                          user_data);
}

/*****************************************************************************/
/* MEID loading (CDMA interface) */

static gchar *
modem_cdma_load_meid_finish (MMIfaceModemCdma *self,
                             GAsyncResult *res,
                             GError **error)
{
    gchar *meid;

    if (g_simple_async_result_propagate_error (G_SIMPLE_ASYNC_RESULT (res), error))
        return NULL;

    meid = g_strdup (g_simple_async_result_get_op_res_gpointer (G_SIMPLE_ASYNC_RESULT (res)));
    mm_dbg ("loaded MEID: %s", meid);
    return meid;
}

static void
modem_cdma_load_meid (MMIfaceModemCdma *_self,
                      GAsyncReadyCallback callback,
                      gpointer user_data)
{
    MMBroadbandModemQmi *self = MM_BROADBAND_MODEM_QMI (_self);
    GSimpleAsyncResult *result;

    result = g_simple_async_result_new (G_OBJECT (self),
                                        callback,
                                        user_data,
                                        modem_cdma_load_meid);

    if (self->priv->meid)
        g_simple_async_result_set_op_res_gpointer (result,
                                                   self->priv->meid,
                                                   NULL);
    else
        g_simple_async_result_set_error (result,
                                         MM_CORE_ERROR,
                                         MM_CORE_ERROR_FAILED,
                                         "Device doesn't report a valid MEID");
    g_simple_async_result_complete_in_idle (result);
    g_object_unref (result);
}

/*****************************************************************************/
/* ESN loading (CDMA interface) */

static gchar *
modem_cdma_load_esn_finish (MMIfaceModemCdma *self,
                            GAsyncResult *res,
                            GError **error)
{
    gchar *esn;

    if (g_simple_async_result_propagate_error (G_SIMPLE_ASYNC_RESULT (res), error))
        return NULL;

    esn = g_strdup (g_simple_async_result_get_op_res_gpointer (G_SIMPLE_ASYNC_RESULT (res)));
    mm_dbg ("loaded ESN: %s", esn);
    return esn;
}

static void
modem_cdma_load_esn (MMIfaceModemCdma *_self,
                     GAsyncReadyCallback callback,
                     gpointer user_data)
{
    MMBroadbandModemQmi *self = MM_BROADBAND_MODEM_QMI (_self);
    GSimpleAsyncResult *result;

    result = g_simple_async_result_new (G_OBJECT (self),
                                        callback,
                                        user_data,
                                        modem_cdma_load_esn);

    if (self->priv->esn)
        g_simple_async_result_set_op_res_gpointer (result,
                                                   self->priv->esn,
                                                   NULL);
    else
        g_simple_async_result_set_error (result,
                                         MM_CORE_ERROR,
                                         MM_CORE_ERROR_FAILED,
                                         "Device doesn't report a valid ESN");
    g_simple_async_result_complete_in_idle (result);
    g_object_unref (result);
}

/*****************************************************************************/
/* Enabling/disabling unsolicited events (3GPP and CDMA interface)
 *
 * If NAS >= 1.8:
 *   - Config Signal Info (only when enabling)
 *   - Register Indications with Signal Info
 *
 * If NAS < 1.8:
 *   - Set Event Report with Signal Strength
 */

typedef struct {
    MMBroadbandModemQmi *self;
    GSimpleAsyncResult *result;
    QmiClientNas *client;
    gboolean enable;
} EnableUnsolicitedEventsContext;

static void
enable_unsolicited_events_context_complete_and_free (EnableUnsolicitedEventsContext *ctx)
{
    g_simple_async_result_complete (ctx->result);
    g_object_unref (ctx->result);
    g_object_unref (ctx->client);
    g_object_unref (ctx->self);
    g_free (ctx);
}

static gboolean
common_enable_disable_unsolicited_events_finish (MMBroadbandModemQmi *self,
                                                 GAsyncResult *res,
                                                 GError **error)
{
    return !g_simple_async_result_propagate_error (G_SIMPLE_ASYNC_RESULT (res), error);
}

static void
ser_signal_strength_ready (QmiClientNas *client,
                           GAsyncResult *res,
                           EnableUnsolicitedEventsContext *ctx)
{
    QmiMessageNasSetEventReportOutput *output = NULL;
    GError *error = NULL;

    output = qmi_client_nas_set_event_report_finish (client, res, &error);
    if (!output) {
        mm_dbg ("QMI operation failed: '%s'", error->message);
        g_error_free (error);
    } else if (!qmi_message_nas_set_event_report_output_get_result (output, &error)) {
        mm_dbg ("Couldn't set event report: '%s'", error->message);
        g_error_free (error);
    }

    if (output)
        qmi_message_nas_set_event_report_output_unref (output);

    /* Just ignore errors for now */
    ctx->self->priv->unsolicited_events_enabled = ctx->enable;
    g_simple_async_result_set_op_res_gboolean (ctx->result, TRUE);
    enable_unsolicited_events_context_complete_and_free (ctx);
}

static void
common_enable_disable_unsolicited_events_signal_strength (EnableUnsolicitedEventsContext *ctx)
{
    /* The device doesn't really like to have many threshold values, so don't
     * grow this array without checking first */
    static const gint8 thresholds_data[] = { -80, -40, 0, 40, 80 };
    QmiMessageNasSetEventReportInput *input;
    GArray *thresholds;

    input = qmi_message_nas_set_event_report_input_new ();

    /* Prepare thresholds, separated 20 each */
    thresholds = g_array_sized_new (FALSE, FALSE, sizeof (gint8), G_N_ELEMENTS (thresholds_data));

    /* Only set thresholds during enable */
    if (ctx->enable)
        g_array_append_vals (thresholds, thresholds_data, G_N_ELEMENTS (thresholds_data));

    qmi_message_nas_set_event_report_input_set_signal_strength_indicator (
        input,
        ctx->enable,
        thresholds,
        NULL);
    g_array_unref (thresholds);
    qmi_client_nas_set_event_report (
        ctx->client,
        input,
        5,
        NULL,
        (GAsyncReadyCallback)ser_signal_strength_ready,
        ctx);
    qmi_message_nas_set_event_report_input_unref (input);
}

static void
ri_signal_info_ready (QmiClientNas *client,
                      GAsyncResult *res,
                      EnableUnsolicitedEventsContext *ctx)
{
    QmiMessageNasRegisterIndicationsOutput *output = NULL;
    GError *error = NULL;

    output = qmi_client_nas_register_indications_finish (client, res, &error);
    if (!output) {
        mm_dbg ("QMI operation failed: '%s'", error->message);
        g_error_free (error);
    } else if (!qmi_message_nas_register_indications_output_get_result (output, &error)) {
        mm_dbg ("Couldn't register indications: '%s'", error->message);
        g_error_free (error);
    }

    if (output)
        qmi_message_nas_register_indications_output_unref (output);

    /* Just ignore errors for now */
    ctx->self->priv->unsolicited_events_enabled = ctx->enable;
    g_simple_async_result_set_op_res_gboolean (ctx->result, TRUE);
    enable_unsolicited_events_context_complete_and_free (ctx);
}

static void
common_enable_disable_unsolicited_events_signal_info (EnableUnsolicitedEventsContext *ctx)
{
    QmiMessageNasRegisterIndicationsInput *input;

    input = qmi_message_nas_register_indications_input_new ();
    qmi_message_nas_register_indications_input_set_signal_info (input, ctx->enable, NULL);
    qmi_client_nas_register_indications (
        ctx->client,
        input,
        5,
        NULL,
        (GAsyncReadyCallback)ri_signal_info_ready,
        ctx);
    qmi_message_nas_register_indications_input_unref (input);
}

static void
config_signal_info_ready (QmiClientNas *client,
                          GAsyncResult *res,
                          EnableUnsolicitedEventsContext *ctx)
{
    QmiMessageNasConfigSignalInfoOutput *output = NULL;
    GError *error = NULL;

    output = qmi_client_nas_config_signal_info_finish (client, res, &error);
    if (!output) {
        mm_dbg ("QMI operation failed: '%s'", error->message);
        g_error_free (error);
    } else if (!qmi_message_nas_config_signal_info_output_get_result (output, &error)) {
        mm_dbg ("Couldn't config signal info: '%s'", error->message);
        g_error_free (error);
    }

    if (output)
        qmi_message_nas_config_signal_info_output_unref (output);

    /* Keep on */
    common_enable_disable_unsolicited_events_signal_info (ctx);
}

static void
common_enable_disable_unsolicited_events_signal_info_config (EnableUnsolicitedEventsContext *ctx)
{
    /* RSSI values go between -105 and -60 for 3GPP technologies,
     * and from -105 to -90 in 3GPP2 technologies (approx). */
    static const gint8 thresholds_data[] = { -100, -97, -95, -92, -90, -85, -80, -75, -70, -65 };
    QmiMessageNasConfigSignalInfoInput *input;
    GArray *thresholds;

    /* Signal info config only to be run when enabling */
    if (!ctx->enable) {
        common_enable_disable_unsolicited_events_signal_info (ctx);
        return;
    }

    input = qmi_message_nas_config_signal_info_input_new ();

    /* Prepare thresholds, separated 20 each */
    thresholds = g_array_sized_new (FALSE, FALSE, sizeof (gint8), G_N_ELEMENTS (thresholds_data));
    g_array_append_vals (thresholds, thresholds_data, G_N_ELEMENTS (thresholds_data));

    qmi_message_nas_config_signal_info_input_set_rssi_threshold (
        input,
        thresholds,
        NULL);
    g_array_unref (thresholds);
    qmi_client_nas_config_signal_info (
        ctx->client,
        input,
        5,
        NULL,
        (GAsyncReadyCallback)config_signal_info_ready,
        ctx);
    qmi_message_nas_config_signal_info_input_unref (input);
}

static void
common_enable_disable_unsolicited_events (MMBroadbandModemQmi *self,
                                          gboolean enable,
                                          GAsyncReadyCallback callback,
                                          gpointer user_data)
{
    EnableUnsolicitedEventsContext *ctx;
    GSimpleAsyncResult *result;
    QmiClient *client = NULL;

    if (!ensure_qmi_client (MM_BROADBAND_MODEM_QMI (self),
                            QMI_SERVICE_NAS, &client,
                            callback, user_data))
        return;

    result = g_simple_async_result_new (G_OBJECT (self),
                                        callback,
                                        user_data,
                                        common_enable_disable_unsolicited_events);

    if (enable == self->priv->unsolicited_events_enabled) {
        mm_dbg ("Unsolicited events already %s; skipping",
                enable ? "enabled" : "disabled");
        g_simple_async_result_set_op_res_gboolean (result, TRUE);
        g_simple_async_result_complete_in_idle (result);
        g_object_unref (result);
        return;
    }

    ctx = g_new0 (EnableUnsolicitedEventsContext, 1);
    ctx->self = g_object_ref (self);
    ctx->client = g_object_ref (client);
    ctx->enable = enable;
    ctx->result = result;

    /* Signal info introduced in NAS 1.8 */
    if (qmi_client_check_version (client, 1, 8))
        common_enable_disable_unsolicited_events_signal_info_config (ctx);
    else
        common_enable_disable_unsolicited_events_signal_strength (ctx);
}

/*****************************************************************************/
/* Enable/Disable unsolicited events (3GPP interface) */

static gboolean
modem_3gpp_enable_disable_unsolicited_events_finish (MMIfaceModem3gpp *self,
                                                     GAsyncResult *res,
                                                     GError **error)
{
    return common_enable_disable_unsolicited_events_finish (MM_BROADBAND_MODEM_QMI (self), res, error);
}

static void
modem_3gpp_disable_unsolicited_events (MMIfaceModem3gpp *self,
                                       GAsyncReadyCallback callback,
                                       gpointer user_data)
{
    common_enable_disable_unsolicited_events (MM_BROADBAND_MODEM_QMI (self),
                                              FALSE,
                                              callback,
                                              user_data);
}

static void
modem_3gpp_enable_unsolicited_events (MMIfaceModem3gpp *self,
                                      GAsyncReadyCallback callback,
                                      gpointer user_data)
{
    common_enable_disable_unsolicited_events (MM_BROADBAND_MODEM_QMI (self),
                                              TRUE,
                                              callback,
                                              user_data);
}

/*****************************************************************************/
/* Enable/Disable unsolicited events (CDMA interface) */

static gboolean
modem_cdma_enable_disable_unsolicited_events_finish (MMIfaceModemCdma *self,
                                                     GAsyncResult *res,
                                                     GError **error)
{
    return common_enable_disable_unsolicited_events_finish (MM_BROADBAND_MODEM_QMI (self), res, error);
}

static void
modem_cdma_disable_unsolicited_events (MMIfaceModemCdma *self,
                                       GAsyncReadyCallback callback,
                                       gpointer user_data)
{
    common_enable_disable_unsolicited_events (MM_BROADBAND_MODEM_QMI (self),
                                              FALSE,
                                              callback,
                                              user_data);
}

static void
modem_cdma_enable_unsolicited_events (MMIfaceModemCdma *self,
                                      GAsyncReadyCallback callback,
                                      gpointer user_data)
{
    common_enable_disable_unsolicited_events (MM_BROADBAND_MODEM_QMI (self),
                                              TRUE,
                                              callback,
                                              user_data);
}

/*****************************************************************************/
/* Setup/Cleanup unsolicited event handlers (3GPP and CDMA interface) */

static gboolean
common_setup_cleanup_unsolicited_events_finish (MMBroadbandModemQmi *self,
                                                GAsyncResult *res,
                                                GError **error)
{
    return !g_simple_async_result_propagate_error (G_SIMPLE_ASYNC_RESULT (res), error);
}

static void
event_report_indication_cb (QmiClientNas *client,
                            QmiIndicationNasEventReportOutput *output,
                            MMBroadbandModemQmi *self)
{
    gint8 signal_strength;
    QmiNasRadioInterface signal_strength_radio_interface;

    if (qmi_indication_nas_event_report_output_get_signal_strength (
            output,
            &signal_strength,
            &signal_strength_radio_interface,
            NULL)) {
        guint8 quality;

        /* This signal strength comes as negative dBms */
        quality = STRENGTH_TO_QUALITY (signal_strength);

        mm_dbg ("Signal strength indication (%s): %d dBm --> %u%%",
                qmi_nas_radio_interface_get_string (signal_strength_radio_interface),
                signal_strength,
                quality);

        mm_iface_modem_update_signal_quality (MM_IFACE_MODEM (self), quality);
    }
}

static void
signal_info_indication_cb (QmiClientNas *client,
                           QmiIndicationNasSignalInfoOutput *output,
                           MMBroadbandModemQmi *self)
{
    gint8 rssi_max = 0;
    gint8 rssi;
    guint8 quality;

    /* We do not report per-technology signal quality, so just get the highest
     * one of the ones reported. TODO: When several technologies are in use, if
     * the indication only contains the data of the one which passed a threshold
     * value, we'll need to have an internal cache of per-technology values, in
     * order to report always the one with the maximum value. */

    if (qmi_indication_nas_signal_info_output_get_cdma_signal_strength (output, &rssi, NULL, NULL)) {
        mm_dbg ("RSSI (CDMA): %d dBm", rssi);
        rssi = MAX (rssi, rssi_max);
    }

    if (qmi_indication_nas_signal_info_output_get_hdr_signal_strength (output, &rssi, NULL, NULL, NULL, NULL)) {
        mm_dbg ("RSSI (HDR): %d dBm", rssi);
        rssi = MAX (rssi, rssi_max);
    }

    if (qmi_indication_nas_signal_info_output_get_gsm_signal_strength (output, &rssi, NULL)) {
        mm_dbg ("RSSI (GSM): %d dBm", rssi);
        rssi = MAX (rssi, rssi_max);
    }

    if (qmi_indication_nas_signal_info_output_get_wcdma_signal_strength (output, &rssi, NULL, NULL)) {
        mm_dbg ("RSSI (WCDMA): %d dBm", rssi);
        rssi = MAX (rssi, rssi_max);
    }

    if (qmi_indication_nas_signal_info_output_get_lte_signal_strength (output, &rssi, NULL, NULL, NULL, NULL)) {
        mm_dbg ("RSSI (LTE): %d dBm", rssi);
        rssi = MAX (rssi, rssi_max);
    }

    /* This RSSI comes as negative dBms */
    quality = STRENGTH_TO_QUALITY (rssi_max);

    mm_dbg ("RSSI: %d dBm --> %u%%", rssi_max, quality);

    mm_iface_modem_update_signal_quality (MM_IFACE_MODEM (self), quality);
}

static void
common_setup_cleanup_unsolicited_events (MMBroadbandModemQmi *self,
                                         gboolean enable,
                                         GAsyncReadyCallback callback,
                                         gpointer user_data)
{
    GSimpleAsyncResult *result;
    QmiClient *client = NULL;

    if (!ensure_qmi_client (MM_BROADBAND_MODEM_QMI (self),
                            QMI_SERVICE_NAS, &client,
                            callback, user_data))
        return;

    result = g_simple_async_result_new (G_OBJECT (self),
                                        callback,
                                        user_data,
                                        common_enable_disable_unsolicited_events);

    if (enable == self->priv->unsolicited_events_setup) {
        mm_dbg ("Unsolicited events already %s; skipping",
                enable ? "setup" : "cleanup");
        g_simple_async_result_set_op_res_gboolean (result, TRUE);
        g_simple_async_result_complete_in_idle (result);
        g_object_unref (result);
        return;
    }

    /* Store new state */
    self->priv->unsolicited_events_setup = enable;

    /* Connect/Disconnect "Event Report" indications */
    if (enable) {
        g_assert (self->priv->event_report_indication_id == 0);
        self->priv->event_report_indication_id =
            g_signal_connect (client,
                              "event-report",
                              G_CALLBACK (event_report_indication_cb),
                              self);
    } else {
        g_assert (self->priv->event_report_indication_id != 0);
        g_signal_handler_disconnect (client, self->priv->event_report_indication_id);
        self->priv->event_report_indication_id = 0;
    }

    /* Connect/Disconnect "Signal Info" indications.
     * Signal info introduced in NAS 1.8 */
    if (qmi_client_check_version (client, 1, 8)) {
        if (enable) {
            g_assert (self->priv->signal_info_indication_id == 0);
            self->priv->signal_info_indication_id =
                g_signal_connect (client,
                                  "signal-info",
                                  G_CALLBACK (signal_info_indication_cb),
                                  self);
        } else {
            g_assert (self->priv->signal_info_indication_id != 0);
            g_signal_handler_disconnect (client, self->priv->signal_info_indication_id);
            self->priv->signal_info_indication_id = 0;
        }
    }

    g_simple_async_result_set_op_res_gboolean (result, TRUE);
    g_simple_async_result_complete_in_idle (result);
    g_object_unref (result);
}

/*****************************************************************************/
/* Enable/Disable unsolicited events (3GPP interface) */

static gboolean
modem_3gpp_setup_cleanup_unsolicited_events_finish (MMIfaceModem3gpp *self,
                                                    GAsyncResult *res,
                                                    GError **error)
{
    return common_setup_cleanup_unsolicited_events_finish (MM_BROADBAND_MODEM_QMI (self), res, error);
}

static void
modem_3gpp_cleanup_unsolicited_events (MMIfaceModem3gpp *self,
                                       GAsyncReadyCallback callback,
                                       gpointer user_data)
{
    common_setup_cleanup_unsolicited_events (MM_BROADBAND_MODEM_QMI (self),
                                             FALSE,
                                             callback,
                                             user_data);
}

static void
modem_3gpp_setup_unsolicited_events (MMIfaceModem3gpp *self,
                                     GAsyncReadyCallback callback,
                                     gpointer user_data)
{
    common_setup_cleanup_unsolicited_events (MM_BROADBAND_MODEM_QMI (self),
                                             TRUE,
                                             callback,
                                             user_data);
}

/*****************************************************************************/
/* Enable/Disable unsolicited events (CDMA interface) */

static gboolean
modem_cdma_setup_cleanup_unsolicited_events_finish (MMIfaceModemCdma *self,
                                                    GAsyncResult *res,
                                                    GError **error)
{
    return common_setup_cleanup_unsolicited_events_finish (MM_BROADBAND_MODEM_QMI (self), res, error);
}

static void
modem_cdma_cleanup_unsolicited_events (MMIfaceModemCdma *self,
                                       GAsyncReadyCallback callback,
                                       gpointer user_data)
{
    common_setup_cleanup_unsolicited_events (MM_BROADBAND_MODEM_QMI (self),
                                             FALSE,
                                             callback,
                                             user_data);
}

static void
modem_cdma_setup_unsolicited_events (MMIfaceModemCdma *self,
                                     GAsyncReadyCallback callback,
                                     gpointer user_data)
{
    common_setup_cleanup_unsolicited_events (MM_BROADBAND_MODEM_QMI (self),
                                             TRUE,
                                             callback,
                                             user_data);
}

/*****************************************************************************/
/* First initialization step */

typedef struct {
    MMBroadbandModem *self;
    GSimpleAsyncResult *result;
    MMQmiPort *qmi;
    QmiService services[32];
    guint service_index;
} InitializationStartedContext;

static void
initialization_started_context_complete_and_free (InitializationStartedContext *ctx)
{
    g_simple_async_result_complete_in_idle (ctx->result);
    if (ctx->qmi)
        g_object_unref (ctx->qmi);
    g_object_unref (ctx->result);
    g_object_unref (ctx->self);
    g_free (ctx);
}

static gpointer
initialization_started_finish (MMBroadbandModem *self,
                               GAsyncResult *res,
                               GError **error)
{
    if (g_simple_async_result_propagate_error (G_SIMPLE_ASYNC_RESULT (res), error))
        return NULL;

    /* Just parent's pointer passed here */
    return g_simple_async_result_get_op_res_gpointer (G_SIMPLE_ASYNC_RESULT (res));
}

static void
parent_initialization_started_ready (MMBroadbandModem *self,
                                     GAsyncResult *res,
                                     InitializationStartedContext *ctx)
{
    gpointer parent_ctx;
    GError *error = NULL;

    parent_ctx = MM_BROADBAND_MODEM_CLASS (mm_broadband_modem_qmi_parent_class)->initialization_started_finish (
        self,
        res,
        &error);
    if (error) {
        g_prefix_error (&error, "Couldn't start parent initialization: ");
        g_simple_async_result_take_error (ctx->result, error);
    } else
        g_simple_async_result_set_op_res_gpointer (ctx->result, parent_ctx, NULL);

    initialization_started_context_complete_and_free (ctx);
}

static void
parent_initialization_started (InitializationStartedContext *ctx)
{
    MM_BROADBAND_MODEM_CLASS (mm_broadband_modem_qmi_parent_class)->initialization_started (
        ctx->self,
        (GAsyncReadyCallback)parent_initialization_started_ready,
        ctx);
}

static void allocate_next_client (InitializationStartedContext *ctx);

static void
qmi_port_allocate_client_ready (MMQmiPort *qmi,
                                GAsyncResult *res,
                                InitializationStartedContext *ctx)
{
    GError *error = NULL;

    if (!mm_qmi_port_allocate_client_finish (qmi, res, &error)) {
        mm_dbg ("Couldn't allocate client for service '%s': %s",
                qmi_service_get_string (ctx->services[ctx->service_index]),
                error->message);
        g_error_free (error);
    }

    ctx->service_index++;
    allocate_next_client (ctx);
}

static void
allocate_next_client (InitializationStartedContext *ctx)
{
    if (ctx->services[ctx->service_index] == QMI_SERVICE_UNKNOWN) {
        /* Done we are, launch parent's callback */
        parent_initialization_started (ctx);
        return;
    }

    /* Otherwise, allocate next client */
    mm_qmi_port_allocate_client (ctx->qmi,
                                 ctx->services[ctx->service_index],
                                 NULL,
                                 (GAsyncReadyCallback)qmi_port_allocate_client_ready,
                                 ctx);
}

static void
qmi_port_open_ready (MMQmiPort *qmi,
                     GAsyncResult *res,
                     InitializationStartedContext *ctx)
{
    GError *error = NULL;

    if (!mm_qmi_port_open_finish (qmi, res, &error)) {
        g_simple_async_result_take_error (ctx->result, error);
        initialization_started_context_complete_and_free (ctx);
        return;
    }

    allocate_next_client (ctx);
}

static void
initialization_started (MMBroadbandModem *self,
                        GAsyncReadyCallback callback,
                        gpointer user_data)
{
    InitializationStartedContext *ctx;

    ctx = g_new0 (InitializationStartedContext, 1);
    ctx->self = g_object_ref (self);
    ctx->result = g_simple_async_result_new (G_OBJECT (self),
                                             callback,
                                             user_data,
                                             initialization_started);
    ctx->qmi = mm_base_modem_get_port_qmi (MM_BASE_MODEM (self));
    g_assert (ctx->qmi);

    if (mm_qmi_port_is_open (ctx->qmi)) {
        /* Nothing to be done, just launch parent's callback */
        parent_initialization_started (ctx);
        return;
    }

    /* Setup services to open */
    ctx->services[0] = QMI_SERVICE_DMS;
    ctx->services[1] = QMI_SERVICE_WDS;
    ctx->services[2] = QMI_SERVICE_NAS;
    ctx->services[3] = QMI_SERVICE_UNKNOWN;

    /* Now open our QMI port */
    mm_qmi_port_open (ctx->qmi,
                      NULL,
                      (GAsyncReadyCallback)qmi_port_open_ready,
                      ctx);
}

/*****************************************************************************/

MMBroadbandModemQmi *
mm_broadband_modem_qmi_new (const gchar *device,
                            const gchar **drivers,
                            const gchar *plugin,
                            guint16 vendor_id,
                            guint16 product_id)
{
    return g_object_new (MM_TYPE_BROADBAND_MODEM_QMI,
                         MM_BASE_MODEM_DEVICE, device,
                         MM_BASE_MODEM_DRIVERS, drivers,
                         MM_BASE_MODEM_PLUGIN, plugin,
                         MM_BASE_MODEM_VENDOR_ID, vendor_id,
                         MM_BASE_MODEM_PRODUCT_ID, product_id,
                         NULL);
}

static void
mm_broadband_modem_qmi_init (MMBroadbandModemQmi *self)
{
    /* Initialize private data */
    self->priv = G_TYPE_INSTANCE_GET_PRIVATE ((self),
                                              MM_TYPE_BROADBAND_MODEM_QMI,
                                              MMBroadbandModemQmiPrivate);

    /* Some initial defaults for when we need to gather info about
     * supported commands/TLVs */
    self->priv->has_mode_preference_in_system_selection_preference = TRUE;
}

static void
finalize (GObject *object)
{
    MMQmiPort *qmi;
    MMBroadbandModemQmi *self = MM_BROADBAND_MODEM_QMI (object);

    qmi = mm_base_modem_peek_port_qmi (MM_BASE_MODEM (self));
    /* If we did open the QMI port during initialization, close it now */
    if (qmi &&
        mm_qmi_port_is_open (qmi)) {
        mm_qmi_port_close (qmi);
    }

    g_free (self->priv->imei);
    g_free (self->priv->meid);
    g_free (self->priv->esn);
    g_free (self->priv->current_operator_id);
    g_free (self->priv->current_operator_description);

    G_OBJECT_CLASS (mm_broadband_modem_qmi_parent_class)->finalize (object);
}

static void
iface_modem_init (MMIfaceModem *iface)
{
    /* Initialization steps */
    iface->load_current_capabilities = modem_load_current_capabilities;
    iface->load_current_capabilities_finish = modem_load_current_capabilities_finish;
    iface->load_manufacturer = modem_load_manufacturer;
    iface->load_manufacturer_finish = modem_load_manufacturer_finish;
    iface->load_model = modem_load_model;
    iface->load_model_finish = modem_load_model_finish;
    iface->load_revision = modem_load_revision;
    iface->load_revision_finish = modem_load_revision_finish;
    iface->load_equipment_identifier = modem_load_equipment_identifier;
    iface->load_equipment_identifier_finish = modem_load_equipment_identifier_finish;
    iface->load_device_identifier = modem_load_device_identifier;
    iface->load_device_identifier_finish = modem_load_device_identifier_finish;
    iface->load_own_numbers = modem_load_own_numbers;
    iface->load_own_numbers_finish = modem_load_own_numbers_finish;
    iface->load_unlock_required = modem_load_unlock_required;
    iface->load_unlock_required_finish = modem_load_unlock_required_finish;
    iface->load_unlock_retries = modem_load_unlock_retries;
    iface->load_unlock_retries_finish = modem_load_unlock_retries_finish;
    iface->load_supported_bands = modem_load_supported_bands;
    iface->load_supported_bands_finish = modem_load_supported_bands_finish;

    /* Enabling/disabling */
    iface->modem_init = NULL;
    iface->modem_init_finish = NULL;
    iface->modem_power_up = modem_power_up;
    iface->modem_power_up_finish = modem_power_up_down_finish;
    iface->modem_after_power_up = NULL;
    iface->modem_after_power_up_finish = NULL;
    iface->setup_flow_control = NULL;
    iface->setup_flow_control_finish = NULL;
    iface->load_supported_charsets = NULL;
    iface->load_supported_charsets_finish = NULL;
    iface->setup_charset = NULL;
    iface->setup_charset_finish = NULL;
    iface->load_allowed_modes = load_allowed_modes;
    iface->load_allowed_modes_finish = load_allowed_modes_finish;
    iface->set_allowed_modes = set_allowed_modes;
    iface->set_allowed_modes_finish = set_allowed_modes_finish;
    iface->modem_power_down = modem_power_down;
    iface->modem_power_down_finish = modem_power_up_down_finish;
    iface->load_signal_quality = load_signal_quality;
    iface->load_signal_quality_finish = load_signal_quality_finish;

    /* Create QMI-specific SIM */
    iface->create_sim = create_sim;
    iface->create_sim_finish = create_sim_finish;

    /* Other actions */
    iface->factory_reset = modem_factory_reset;
    iface->factory_reset_finish = modem_factory_reset_finish;
}

static void
iface_modem_3gpp_init (MMIfaceModem3gpp *iface)
{
    /* Initialization steps */
    iface->load_imei = modem_3gpp_load_imei;
    iface->load_imei_finish = modem_3gpp_load_imei_finish;
    iface->load_enabled_facility_locks = modem_3gpp_load_enabled_facility_locks;
    iface->load_enabled_facility_locks_finish = modem_3gpp_load_enabled_facility_locks_finish;

    /* Enabling/Disabling steps */
    iface->setup_unsolicited_events = modem_3gpp_setup_unsolicited_events;
    iface->setup_unsolicited_events_finish = modem_3gpp_setup_cleanup_unsolicited_events_finish;
    iface->cleanup_unsolicited_events = modem_3gpp_cleanup_unsolicited_events;
    iface->cleanup_unsolicited_events_finish = modem_3gpp_setup_cleanup_unsolicited_events_finish;
    iface->enable_unsolicited_events = modem_3gpp_enable_unsolicited_events;
    iface->enable_unsolicited_events_finish = modem_3gpp_enable_disable_unsolicited_events_finish;
    iface->disable_unsolicited_events = modem_3gpp_disable_unsolicited_events;
    iface->disable_unsolicited_events_finish = modem_3gpp_enable_disable_unsolicited_events_finish;
    iface->setup_unsolicited_registration_events = modem_3gpp_setup_unsolicited_registration_events;
    iface->setup_unsolicited_registration_events_finish = modem_3gpp_setup_cleanup_unsolicited_registration_events_finish;
    iface->cleanup_unsolicited_registration_events = modem_3gpp_cleanup_unsolicited_registration_events;
    iface->cleanup_unsolicited_registration_events_finish = modem_3gpp_setup_cleanup_unsolicited_registration_events_finish;
    iface->enable_unsolicited_registration_events = modem_3gpp_enable_unsolicited_registration_events;
    iface->enable_unsolicited_registration_events_finish = modem_3gpp_enable_disable_unsolicited_registration_events_finish;
    iface->disable_unsolicited_registration_events = modem_3gpp_disable_unsolicited_registration_events;
    iface->disable_unsolicited_registration_events_finish = modem_3gpp_enable_disable_unsolicited_registration_events_finish;

    /* Other actions */
    iface->scan_networks = modem_3gpp_scan_networks;
    iface->scan_networks_finish = modem_3gpp_scan_networks_finish;
    iface->register_in_network = modem_3gpp_register_in_network;
    iface->register_in_network_finish = modem_3gpp_register_in_network_finish;
    iface->run_registration_checks = modem_3gpp_run_registration_checks;
    iface->run_registration_checks_finish = modem_3gpp_run_registration_checks_finish;
    iface->load_operator_code = modem_3gpp_load_operator_code;
    iface->load_operator_code_finish = modem_3gpp_load_operator_code_finish;
    iface->load_operator_name = modem_3gpp_load_operator_name;
    iface->load_operator_name_finish = modem_3gpp_load_operator_name_finish;
}

static void
iface_modem_cdma_init (MMIfaceModemCdma *iface)
{
    iface->load_meid = modem_cdma_load_meid;
    iface->load_meid_finish = modem_cdma_load_meid_finish;
    iface->load_esn = modem_cdma_load_esn;
    iface->load_esn_finish = modem_cdma_load_esn_finish;

    /* Enabling/Disabling steps */
    iface->setup_unsolicited_events = modem_cdma_setup_unsolicited_events;
    iface->setup_unsolicited_events_finish = modem_cdma_setup_cleanup_unsolicited_events_finish;
    iface->cleanup_unsolicited_events = modem_cdma_cleanup_unsolicited_events;
    iface->cleanup_unsolicited_events_finish = modem_cdma_setup_cleanup_unsolicited_events_finish;
    iface->enable_unsolicited_events = modem_cdma_enable_unsolicited_events;
    iface->enable_unsolicited_events_finish = modem_cdma_enable_disable_unsolicited_events_finish;
    iface->disable_unsolicited_events = modem_cdma_disable_unsolicited_events;
    iface->disable_unsolicited_events_finish = modem_cdma_enable_disable_unsolicited_events_finish;

    /* Other actions */
    iface->run_registration_checks = modem_cdma_run_registration_checks;
    iface->run_registration_checks_finish = modem_cdma_run_registration_checks_finish;
}

static void
mm_broadband_modem_qmi_class_init (MMBroadbandModemQmiClass *klass)
{
    GObjectClass *object_class = G_OBJECT_CLASS (klass);
    MMBroadbandModemClass *broadband_modem_class = MM_BROADBAND_MODEM_CLASS (klass);

    g_type_class_add_private (object_class, sizeof (MMBroadbandModemQmiPrivate));

    object_class->finalize = finalize;

    broadband_modem_class->initialization_started = initialization_started;
    broadband_modem_class->initialization_started_finish = initialization_started_finish;
}

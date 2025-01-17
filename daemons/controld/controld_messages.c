/*
 * Copyright 2004-2019 the Pacemaker project contributors
 *
 * The version control history for this file may have further details.
 *
 * This source code is licensed under the GNU General Public License version 2
 * or later (GPLv2+) WITHOUT ANY WARRANTY.
 */

#include <crm_internal.h>

#include <sys/param.h>
#include <string.h>
#include <time.h>

#include <crm/crm.h>
#include <crm/msg_xml.h>
#include <crm/common/xml.h>
#include <crm/cluster/internal.h>
#include <crm/cib.h>
#include <crm/common/ipcs.h>

#include <pacemaker-controld.h>

GListPtr fsa_message_queue = NULL;
extern void crm_shutdown(int nsig);

void handle_response(xmlNode * stored_msg);
enum crmd_fsa_input handle_request(xmlNode * stored_msg, enum crmd_fsa_cause cause);
enum crmd_fsa_input handle_shutdown_request(xmlNode * stored_msg);

#define ROUTER_RESULT(x)	crm_trace("Router result: %s", x)

/* debug only, can wrap all it likes */
int last_data_id = 0;

void
register_fsa_error_adv(enum crmd_fsa_cause cause, enum crmd_fsa_input input,
                       fsa_data_t * cur_data, void *new_data, const char *raised_from)
{
    /* save the current actions if any */
    if (fsa_actions != A_NOTHING) {
        register_fsa_input_adv(cur_data ? cur_data->fsa_cause : C_FSA_INTERNAL,
                               I_NULL, cur_data ? cur_data->data : NULL,
                               fsa_actions, TRUE, __FUNCTION__);
    }

    /* reset the action list */
    crm_info("Resetting the current action list");
    fsa_dump_actions(fsa_actions, "Drop");
    fsa_actions = A_NOTHING;

    /* register the error */
    register_fsa_input_adv(cause, input, new_data, A_NOTHING, TRUE, raised_from);
}

int
register_fsa_input_adv(enum crmd_fsa_cause cause, enum crmd_fsa_input input,
                       void *data, long long with_actions,
                       gboolean prepend, const char *raised_from)
{
    unsigned old_len = g_list_length(fsa_message_queue);
    fsa_data_t *fsa_data = NULL;

    if (raised_from == NULL) {
        raised_from = "<unknown>";
    }

    if (input == I_NULL && with_actions == A_NOTHING /* && data == NULL */ ) {
        /* no point doing anything */
        crm_err("Cannot add entry to queue: no input and no action");
        return 0;
    }

    if (input == I_WAIT_FOR_EVENT) {
        do_fsa_stall = TRUE;
        crm_debug("Stalling the FSA pending further input: source=%s cause=%s data=%p queue=%d",
                  raised_from, fsa_cause2string(cause), data, old_len);

        if (old_len > 0) {
            fsa_dump_queue(LOG_TRACE);
            prepend = FALSE;
        }

        if (data == NULL) {
            fsa_actions |= with_actions;
            fsa_dump_actions(with_actions, "Restored");
            return 0;
        }

        /* Store everything in the new event and reset fsa_actions */
        with_actions |= fsa_actions;
        fsa_actions = A_NOTHING;
    }

    last_data_id++;
    crm_trace("%s %s FSA input %d (%s) (cause=%s) %s data",
              raised_from, prepend ? "prepended" : "appended", last_data_id,
              fsa_input2string(input), fsa_cause2string(cause), data ? "with" : "without");

    fsa_data = calloc(1, sizeof(fsa_data_t));
    fsa_data->id = last_data_id;
    fsa_data->fsa_input = input;
    fsa_data->fsa_cause = cause;
    fsa_data->origin = raised_from;
    fsa_data->data = NULL;
    fsa_data->data_type = fsa_dt_none;
    fsa_data->actions = with_actions;

    if (with_actions != A_NOTHING) {
        crm_trace("Adding actions %.16llx to input", with_actions);
    }

    if (data != NULL) {
        switch (cause) {
            case C_FSA_INTERNAL:
            case C_CRMD_STATUS_CALLBACK:
            case C_IPC_MESSAGE:
            case C_HA_MESSAGE:
                crm_trace("Copying %s data from %s as a HA msg",
                          fsa_cause2string(cause), raised_from);
                CRM_CHECK(((ha_msg_input_t *) data)->msg != NULL,
                          crm_err("Bogus data from %s", raised_from));
                fsa_data->data = copy_ha_msg_input(data);
                fsa_data->data_type = fsa_dt_ha_msg;
                break;

            case C_LRM_OP_CALLBACK:
                crm_trace("Copying %s data from %s as lrmd_event_data_t",
                          fsa_cause2string(cause), raised_from);
                fsa_data->data = lrmd_copy_event((lrmd_event_data_t *) data);
                fsa_data->data_type = fsa_dt_lrm;
                break;

            case C_TIMER_POPPED:
            case C_SHUTDOWN:
            case C_UNKNOWN:
            case C_STARTUP:
                crm_err("Copying %s data (from %s)"
                        " not yet implemented", fsa_cause2string(cause), raised_from);
                crmd_exit(CRM_EX_SOFTWARE);
                break;
        }
        crm_trace("%s data copied", fsa_cause2string(fsa_data->fsa_cause));
    }

    /* make sure to free it properly later */
    if (prepend) {
        crm_trace("Prepending input");
        fsa_message_queue = g_list_prepend(fsa_message_queue, fsa_data);
    } else {
        fsa_message_queue = g_list_append(fsa_message_queue, fsa_data);
    }

    crm_trace("Queue len: %d", g_list_length(fsa_message_queue));

    /* fsa_dump_queue(LOG_TRACE); */

    if (old_len == g_list_length(fsa_message_queue)) {
        crm_err("Couldn't add message to the queue");
    }

    if (fsa_source && input != I_WAIT_FOR_EVENT) {
        crm_trace("Triggering FSA: %s", __FUNCTION__);
        mainloop_set_trigger(fsa_source);
    }
    return last_data_id;
}

void
fsa_dump_queue(int log_level)
{
    int offset = 0;
    GListPtr lpc = NULL;

    for (lpc = fsa_message_queue; lpc != NULL; lpc = lpc->next) {
        fsa_data_t *data = (fsa_data_t *) lpc->data;

        do_crm_log_unlikely(log_level,
                            "queue[%d.%d]: input %s raised by %s(%p.%d)\t(cause=%s)",
                            offset++, data->id, fsa_input2string(data->fsa_input),
                            data->origin, data->data, data->data_type,
                            fsa_cause2string(data->fsa_cause));
    }
}

ha_msg_input_t *
copy_ha_msg_input(ha_msg_input_t * orig)
{
    ha_msg_input_t *copy = NULL;
    xmlNodePtr data = NULL;

    if (orig != NULL) {
        crm_trace("Copy msg");
        data = copy_xml(orig->msg);

    } else {
        crm_trace("No message to copy");
    }
    copy = new_ha_msg_input(data);
    if (orig && orig->msg != NULL) {
        CRM_CHECK(copy->msg != NULL, crm_err("copy failed"));
    }
    return copy;
}

void
delete_fsa_input(fsa_data_t * fsa_data)
{
    lrmd_event_data_t *op = NULL;
    xmlNode *foo = NULL;

    if (fsa_data == NULL) {
        return;
    }
    crm_trace("About to free %s data", fsa_cause2string(fsa_data->fsa_cause));

    if (fsa_data->data != NULL) {
        switch (fsa_data->data_type) {
            case fsa_dt_ha_msg:
                delete_ha_msg_input(fsa_data->data);
                break;

            case fsa_dt_xml:
                foo = fsa_data->data;
                free_xml(foo);
                break;

            case fsa_dt_lrm:
                op = (lrmd_event_data_t *) fsa_data->data;
                lrmd_free_event(op);
                break;

            case fsa_dt_none:
                if (fsa_data->data != NULL) {
                    crm_err("Don't know how to free %s data from %s",
                            fsa_cause2string(fsa_data->fsa_cause), fsa_data->origin);
                    crmd_exit(CRM_EX_SOFTWARE);
                }
                break;
        }
        crm_trace("%s data freed", fsa_cause2string(fsa_data->fsa_cause));
    }

    free(fsa_data);
}

/* returns the next message */
fsa_data_t *
get_message(void)
{
    fsa_data_t *message = g_list_nth_data(fsa_message_queue, 0);

    fsa_message_queue = g_list_remove(fsa_message_queue, message);
    crm_trace("Processing input %d", message->id);
    return message;
}

/* returns the current head of the FIFO queue */
gboolean
is_message(void)
{
    return (g_list_length(fsa_message_queue) > 0);
}

void *
fsa_typed_data_adv(fsa_data_t * fsa_data, enum fsa_data_type a_type, const char *caller)
{
    void *ret_val = NULL;

    if (fsa_data == NULL) {
        crm_err("%s: No FSA data available", caller);

    } else if (fsa_data->data == NULL) {
        crm_err("%s: No message data available. Origin: %s", caller, fsa_data->origin);

    } else if (fsa_data->data_type != a_type) {
        crm_crit("%s: Message data was the wrong type! %d vs. requested=%d.  Origin: %s",
                 caller, fsa_data->data_type, a_type, fsa_data->origin);
        CRM_ASSERT(fsa_data->data_type == a_type);
    } else {
        ret_val = fsa_data->data;
    }

    return ret_val;
}

/*	A_MSG_ROUTE	*/
void
do_msg_route(long long action,
             enum crmd_fsa_cause cause,
             enum crmd_fsa_state cur_state,
             enum crmd_fsa_input current_input, fsa_data_t * msg_data)
{
    ha_msg_input_t *input = fsa_typed_data(fsa_dt_ha_msg);

    route_message(msg_data->fsa_cause, input->msg);
}

void
route_message(enum crmd_fsa_cause cause, xmlNode * input)
{
    ha_msg_input_t fsa_input;
    enum crmd_fsa_input result = I_NULL;

    fsa_input.msg = input;
    CRM_CHECK(cause == C_IPC_MESSAGE || cause == C_HA_MESSAGE, return);

    /* try passing the buck first */
    if (relay_message(input, cause == C_IPC_MESSAGE)) {
        return;
    }

    /* handle locally */
    result = handle_message(input, cause);

    /* done or process later? */
    switch (result) {
        case I_NULL:
        case I_CIB_OP:
        case I_ROUTER:
        case I_NODE_JOIN:
        case I_JOIN_REQUEST:
        case I_JOIN_RESULT:
            break;
        default:
            /* Defering local processing of message */
            register_fsa_input_later(cause, result, &fsa_input);
            return;
    }

    if (result != I_NULL) {
        /* add to the front of the queue */
        register_fsa_input(cause, result, &fsa_input);
    }
}

gboolean
relay_message(xmlNode * msg, gboolean originated_locally)
{
    int dest = 1;
    int is_for_dc = 0;
    int is_for_dcib = 0;
    int is_for_te = 0;
    int is_for_crm = 0;
    int is_for_cib = 0;
    int is_local = 0;
    gboolean processing_complete = FALSE;
    const char *host_to = crm_element_value(msg, F_CRM_HOST_TO);
    const char *sys_to = crm_element_value(msg, F_CRM_SYS_TO);
    const char *sys_from = crm_element_value(msg, F_CRM_SYS_FROM);
    const char *type = crm_element_value(msg, F_TYPE);
    const char *task = crm_element_value(msg, F_CRM_TASK);
    const char *msg_error = NULL;

    crm_trace("Routing message %s", crm_element_value(msg, XML_ATTR_REFERENCE));

    if (msg == NULL) {
        msg_error = "Cannot route empty message";

    } else if (safe_str_eq(task, CRM_OP_HELLO)) {
        /* quietly ignore */
        processing_complete = TRUE;

    } else if (safe_str_neq(type, T_CRM)) {
        msg_error = "Bad message type";

    } else if (sys_to == NULL) {
        msg_error = "Bad message destination: no subsystem";
    }

    if (msg_error != NULL) {
        processing_complete = TRUE;
        crm_err("%s", msg_error);
        crm_log_xml_warn(msg, "bad msg");
    }

    if (processing_complete) {
        return TRUE;
    }

    processing_complete = TRUE;

    is_for_dc = (strcasecmp(CRM_SYSTEM_DC, sys_to) == 0);
    is_for_dcib = (strcasecmp(CRM_SYSTEM_DCIB, sys_to) == 0);
    is_for_te = (strcasecmp(CRM_SYSTEM_TENGINE, sys_to) == 0);
    is_for_cib = (strcasecmp(CRM_SYSTEM_CIB, sys_to) == 0);
    is_for_crm = (strcasecmp(CRM_SYSTEM_CRMD, sys_to) == 0);

    is_local = 0;
    if (host_to == NULL || strlen(host_to) == 0) {
        if (is_for_dc || is_for_te) {
            is_local = 0;

        } else if (is_for_crm) {
            if (safe_str_eq(task, CRM_OP_NODE_INFO)) {
                /* Node info requests do not specify a host, which is normally
                 * treated as "all hosts", because the whole point is that the
                 * client doesn't know the local node name. Always handle these
                 * requests locally.
                 */
                is_local = 1;
            } else {
                is_local = !originated_locally;
            }

        } else {
            is_local = 1;
        }

    } else if (safe_str_eq(fsa_our_uname, host_to)) {
        is_local = 1;
    }

    if (is_for_dc || is_for_dcib || is_for_te) {
        if (AM_I_DC && is_for_te) {
            ROUTER_RESULT("Message result: Local relay");
            send_msg_via_ipc(msg, sys_to);

        } else if (AM_I_DC) {
            ROUTER_RESULT("Message result: DC/controller process");
            processing_complete = FALSE;        /* more to be done by caller */
        } else if (originated_locally && safe_str_neq(sys_from, CRM_SYSTEM_PENGINE)
                   && safe_str_neq(sys_from, CRM_SYSTEM_TENGINE)) {

            /* Neither the TE nor the scheduler should be sending messages
             * to DCs on other nodes. By definition, if we are no longer the DC,
             * then the scheduler's or TE's data should be discarded.
             */

#if SUPPORT_COROSYNC
            if (is_corosync_cluster()) {
                dest = text2msg_type(sys_to);
            }
#endif
            ROUTER_RESULT("Message result: External relay to DC");
            send_cluster_message(host_to ? crm_get_peer(0, host_to) : NULL, dest, msg, TRUE);

        } else {
            /* discard */
            ROUTER_RESULT("Message result: Discard, not DC");
        }

    } else if (is_local && (is_for_crm || is_for_cib)) {
        ROUTER_RESULT("Message result: controller process");
        processing_complete = FALSE;    /* more to be done by caller */

    } else if (is_local) {
        ROUTER_RESULT("Message result: Local relay");
        send_msg_via_ipc(msg, sys_to);

    } else {
        crm_node_t *node_to = NULL;

#if SUPPORT_COROSYNC
        if (is_corosync_cluster()) {
            dest = text2msg_type(sys_to);

            if (dest == crm_msg_none || dest > crm_msg_stonith_ng) {
                dest = crm_msg_crmd;
            }
        }
#endif

        if (host_to) {
            node_to = crm_find_peer(0, host_to);
            if (node_to == NULL) {
               crm_err("Cannot route message to unknown node %s", host_to);
               return TRUE;
            }
        }

        ROUTER_RESULT("Message result: External relay");
        send_cluster_message(host_to ? node_to : NULL, dest, msg, TRUE);
    }

    return processing_complete;
}

static gboolean
process_hello_message(xmlNode * hello,
                      char **client_name, char **major_version, char **minor_version)
{
    const char *local_client_name;
    const char *local_major_version;
    const char *local_minor_version;

    *client_name = NULL;
    *major_version = NULL;
    *minor_version = NULL;

    if (hello == NULL) {
        return FALSE;
    }

    local_client_name = crm_element_value(hello, "client_name");
    local_major_version = crm_element_value(hello, "major_version");
    local_minor_version = crm_element_value(hello, "minor_version");

    if (local_client_name == NULL || strlen(local_client_name) == 0) {
        crm_err("Hello message was not valid (field %s not found)", "client name");
        return FALSE;

    } else if (local_major_version == NULL || strlen(local_major_version) == 0) {
        crm_err("Hello message was not valid (field %s not found)", "major version");
        return FALSE;

    } else if (local_minor_version == NULL || strlen(local_minor_version) == 0) {
        crm_err("Hello message was not valid (field %s not found)", "minor version");
        return FALSE;
    }

    *client_name = strdup(local_client_name);
    *major_version = strdup(local_major_version);
    *minor_version = strdup(local_minor_version);

    crm_trace("Hello message ok");
    return TRUE;
}

gboolean
crmd_authorize_message(xmlNode * client_msg, crm_client_t * curr_client, const char *proxy_session)
{
    char *client_name = NULL;
    char *major_version = NULL;
    char *minor_version = NULL;
    gboolean auth_result = FALSE;

    xmlNode *xml = NULL;
    const char *op = crm_element_value(client_msg, F_CRM_TASK);
    const char *uuid = curr_client ? curr_client->id : proxy_session;

    if (uuid == NULL) {
        crm_warn("Message [%s] not authorized", crm_element_value(client_msg, XML_ATTR_REFERENCE));
        return FALSE;

    } else if (safe_str_neq(CRM_OP_HELLO, op)) {
        return TRUE;
    }

    xml = get_message_xml(client_msg, F_CRM_DATA);
    auth_result = process_hello_message(xml, &client_name, &major_version, &minor_version);

    if (auth_result == TRUE) {
        if (client_name == NULL) {
            crm_err("Bad client details (client_name=%s, uuid=%s)",
                    crm_str(client_name), uuid);
            auth_result = FALSE;
        }
    }

    if (auth_result == TRUE) {
        /* check version */
        int mav = atoi(major_version);
        int miv = atoi(minor_version);

        crm_trace("Checking client version number");
        if (mav < 0 || miv < 0) {
            crm_err("Client version (%d:%d) is not acceptable", mav, miv);
            auth_result = FALSE;
        }
    }

    if (auth_result == TRUE) {
        crm_trace("Accepted client %s", client_name);
        if (curr_client) {
            curr_client->userdata = strdup(client_name);
        }

        crm_trace("Triggering FSA: %s", __FUNCTION__);
        mainloop_set_trigger(fsa_source);

    } else {
        crm_warn("Rejected client logon request");
        if (curr_client) {
            qb_ipcs_disconnect(curr_client->ipcs);
        }
    }

    free(minor_version);
    free(major_version);
    free(client_name);

    /* hello messages should never be processed further */
    return FALSE;
}

enum crmd_fsa_input
handle_message(xmlNode * msg, enum crmd_fsa_cause cause)
{
    const char *type = NULL;

    CRM_CHECK(msg != NULL, return I_NULL);

    type = crm_element_value(msg, F_CRM_MSG_TYPE);
    if (crm_str_eq(type, XML_ATTR_REQUEST, TRUE)) {
        return handle_request(msg, cause);

    } else if (crm_str_eq(type, XML_ATTR_RESPONSE, TRUE)) {
        handle_response(msg);
        return I_NULL;
    }

    crm_err("Unknown message type: %s", type);
    return I_NULL;
}

static enum crmd_fsa_input
handle_failcount_op(xmlNode * stored_msg)
{
    const char *rsc = NULL;
    const char *uname = NULL;
    const char *op = NULL;
    const char *interval_ms_s = NULL;
    char *interval_spec = NULL;
    guint interval_ms = 0;
    gboolean is_remote_node = FALSE;
    xmlNode *xml_op = get_message_xml(stored_msg, F_CRM_DATA);

    if (xml_op) {
        xmlNode *xml_rsc = first_named_child(xml_op, XML_CIB_TAG_RESOURCE);
        xmlNode *xml_attrs = first_named_child(xml_op, XML_TAG_ATTRS);

        if (xml_rsc) {
            rsc = ID(xml_rsc);
        }
        if (xml_attrs) {
            op = crm_element_value(xml_attrs,
                                   CRM_META "_" XML_RSC_ATTR_CLEAR_OP);
            interval_ms_s = crm_element_value(xml_attrs,
                                              CRM_META "_" XML_RSC_ATTR_CLEAR_INTERVAL);
            interval_ms = crm_parse_ms(interval_ms_s);
        }
    }
    uname = crm_element_value(xml_op, XML_LRM_ATTR_TARGET);

    if ((rsc == NULL) || (uname == NULL)) {
        crm_log_xml_warn(stored_msg, "invalid failcount op");
        return I_NULL;
    }

    if (crm_element_value(xml_op, XML_LRM_ATTR_ROUTER_NODE)) {
        is_remote_node = TRUE;
    }

    if (interval_ms) {
        interval_spec = crm_strdup_printf("%ums", interval_ms);
    }
    update_attrd_clear_failures(uname, rsc, op, interval_spec, is_remote_node);
    free(interval_spec);

    lrm_clear_last_failure(rsc, uname, op, interval_ms);

    return I_NULL;
}

/*!
 * \brief Handle a CRM_OP_REMOTE_STATE message by updating remote peer cache
 *
 * \param[in] msg  Message XML
 *
 * \return Next FSA input
 */
static enum crmd_fsa_input
handle_remote_state(xmlNode *msg)
{
    const char *remote_uname = ID(msg);
    const char *remote_is_up = crm_element_value(msg, XML_NODE_IN_CLUSTER);
    crm_node_t *remote_peer;

    CRM_CHECK(remote_uname && remote_is_up, return I_NULL);

    remote_peer = crm_remote_peer_get(remote_uname);
    CRM_CHECK(remote_peer, return I_NULL);

    crm_update_peer_state(__FUNCTION__, remote_peer,
                          crm_is_true(remote_is_up)?
                          CRM_NODE_MEMBER : CRM_NODE_LOST, 0);
    return I_NULL;
}

/*!
 * \brief Handle a CRM_OP_PING message
 *
 * \param[in] msg  Message XML
 *
 * \return Next FSA input
 */
static enum crmd_fsa_input
handle_ping(xmlNode *msg)
{
    const char *value = NULL;
    xmlNode *ping = NULL;

    // Build reply

    ping = create_xml_node(NULL, XML_CRM_TAG_PING);
    value = crm_element_value(msg, F_CRM_SYS_TO);
    crm_xml_add(ping, XML_PING_ATTR_SYSFROM, value);

    // Add controller state
    value = fsa_state2string(fsa_state);
    crm_xml_add(ping, XML_PING_ATTR_CRMDSTATE, value);
    crm_notice("Current ping state: %s", value); // CTS needs this

    // Add controller health
    // @TODO maybe do some checks to determine meaningful status
    crm_xml_add(ping, XML_PING_ATTR_STATUS, "ok");

    // Send reply
    msg = create_reply(msg, ping);
    free_xml(ping);
    if (msg) {
        (void) relay_message(msg, TRUE);
        free_xml(msg);
    }

    // Nothing further to do
    return I_NULL;
}

/*!
 * \brief Handle a CRM_OP_NODE_INFO request
 *
 * \param[in] msg  Message XML
 *
 * \return Next FSA input
 */
static enum crmd_fsa_input
handle_node_info_request(xmlNode *msg)
{
    const char *value = NULL;
    crm_node_t *node = NULL;
    int node_id = 0;
    xmlNode *reply = NULL;

    // Build reply

    reply = create_xml_node(NULL, XML_CIB_TAG_NODE);
    crm_xml_add(reply, XML_PING_ATTR_SYSFROM, CRM_SYSTEM_CRMD);

    // Add whether current partition has quorum
    crm_xml_add_boolean(reply, XML_ATTR_HAVE_QUORUM, fsa_has_quorum);

    // Check whether client requested node info by ID and/or name
    crm_element_value_int(msg, XML_ATTR_ID, &node_id);
    if (node_id < 0) {
        node_id = 0;
    }
    value = crm_element_value(msg, XML_ATTR_UNAME);

    // Default to local node if none given
    if ((node_id == 0) && (value == NULL)) {
        value = fsa_our_uname;
    }

    node = crm_find_peer_full(node_id, value, CRM_GET_PEER_ANY);
    if (node) {
        crm_xml_add_int(reply, XML_ATTR_ID, node->id);
        crm_xml_add(reply, XML_ATTR_UUID, node->uuid);
        crm_xml_add(reply, XML_ATTR_UNAME, node->uname);
        crm_xml_add(reply, XML_NODE_IS_PEER, node->state);
        crm_xml_add_boolean(reply, XML_NODE_IS_REMOTE,
                            node->flags & crm_remote_node);
    }

    // Send reply
    msg = create_reply(msg, reply);
    free_xml(reply);
    if (msg) {
        (void) relay_message(msg, TRUE);
        free_xml(msg);
    }

    // Nothing further to do
    return I_NULL;
}

static void
verify_feature_set(xmlNode *msg)
{
    const char *dc_version = crm_element_value(msg, XML_ATTR_CRM_VERSION);

    if (dc_version == NULL) {
        /* All we really know is that the DC feature set is older than 3.1.0,
         * but that's also all that really matters.
         */
        dc_version = "3.0.14";
    }

    if (feature_set_compatible(dc_version, CRM_FEATURE_SET)) {
        crm_trace("Local feature set (%s) is compatible with DC's (%s)",
                  CRM_FEATURE_SET, dc_version);
    } else {
        crm_err("Local feature set (%s) is incompatible with DC's (%s)",
                CRM_FEATURE_SET, dc_version);

        // Nothing is likely to improve without administrator involvement
        set_bit(fsa_input_register, R_STAYDOWN);
        crmd_exit(CRM_EX_FATAL);
    }
}

enum crmd_fsa_input
handle_request(xmlNode * stored_msg, enum crmd_fsa_cause cause)
{
    xmlNode *msg = NULL;
    const char *op = crm_element_value(stored_msg, F_CRM_TASK);

    /* Optimize this for the DC - it has the most to do */

    if (op == NULL) {
        crm_log_xml_err(stored_msg, "Bad message");
        return I_NULL;
    }

    if (strcmp(op, CRM_OP_SHUTDOWN_REQ) == 0) {
        const char *from = crm_element_value(stored_msg, F_CRM_HOST_FROM);
        crm_node_t *node = crm_find_peer(0, from);

        crm_update_peer_expected(__FUNCTION__, node, CRMD_JOINSTATE_DOWN);
        if(AM_I_DC == FALSE) {
            return I_NULL; /* Done */
        }
    }

    /*========== DC-Only Actions ==========*/
    if (AM_I_DC) {
        if (strcmp(op, CRM_OP_JOIN_ANNOUNCE) == 0) {
            return I_NODE_JOIN;

        } else if (strcmp(op, CRM_OP_JOIN_REQUEST) == 0) {
            return I_JOIN_REQUEST;

        } else if (strcmp(op, CRM_OP_JOIN_CONFIRM) == 0) {
            return I_JOIN_RESULT;

        } else if (strcmp(op, CRM_OP_SHUTDOWN) == 0) {
            const char *host_from = crm_element_value(stored_msg, F_CRM_HOST_FROM);
            gboolean dc_match = safe_str_eq(host_from, fsa_our_dc);

            if (is_set(fsa_input_register, R_SHUTDOWN)) {
                crm_info("Shutting ourselves down (DC)");
                return I_STOP;

            } else if (dc_match) {
                crm_err("We didn't ask to be shut down, yet our"
                        " TE is telling us to. Better get out now!");
                return I_TERMINATE;

            } else if (fsa_state != S_STOPPING) {
                crm_err("Another node is asking us to shutdown" " but we think we're ok.");
                return I_ELECTION;
            }

        } else if (strcmp(op, CRM_OP_SHUTDOWN_REQ) == 0) {
            /* a slave wants to shut down */
            /* create cib fragment and add to message */
            return handle_shutdown_request(stored_msg);

        } else if (strcmp(op, CRM_OP_REMOTE_STATE) == 0) {
            /* a remote connection host is letting us know the node state */
            return handle_remote_state(stored_msg);
        }
    }

    /*========== common actions ==========*/
    if (strcmp(op, CRM_OP_NOVOTE) == 0) {
        ha_msg_input_t fsa_input;

        fsa_input.msg = stored_msg;
        register_fsa_input_adv(C_HA_MESSAGE, I_NULL, &fsa_input,
                               A_ELECTION_COUNT | A_ELECTION_CHECK, FALSE, __FUNCTION__);

    } else if (strcmp(op, CRM_OP_THROTTLE) == 0) {
        throttle_update(stored_msg);
        if (AM_I_DC && transition_graph != NULL) {
            if (transition_graph->complete == FALSE) {
                crm_debug("The throttle changed. Trigger a graph.");
                trigger_graph();
            }
        }
        return I_NULL;

    } else if (strcmp(op, CRM_OP_CLEAR_FAILCOUNT) == 0) {
        return handle_failcount_op(stored_msg);

    } else if (strcmp(op, CRM_OP_VOTE) == 0) {
        /* count the vote and decide what to do after that */
        ha_msg_input_t fsa_input;

        fsa_input.msg = stored_msg;
        register_fsa_input_adv(C_HA_MESSAGE, I_NULL, &fsa_input,
                               A_ELECTION_COUNT | A_ELECTION_CHECK, FALSE, __FUNCTION__);

        /* Sometimes we _must_ go into S_ELECTION */
        if (fsa_state == S_HALT) {
            crm_debug("Forcing an election from S_HALT");
            return I_ELECTION;
#if 0
        } else if (AM_I_DC) {
            /* This is the old way of doing things but what is gained? */
            return I_ELECTION;
#endif
        }

    } else if (strcmp(op, CRM_OP_JOIN_OFFER) == 0) {
        verify_feature_set(stored_msg);
        crm_debug("Raising I_JOIN_OFFER: join-%s", crm_element_value(stored_msg, F_CRM_JOIN_ID));
        return I_JOIN_OFFER;

    } else if (strcmp(op, CRM_OP_JOIN_ACKNAK) == 0) {
        crm_debug("Raising I_JOIN_RESULT: join-%s", crm_element_value(stored_msg, F_CRM_JOIN_ID));
        return I_JOIN_RESULT;

    } else if (strcmp(op, CRM_OP_LRM_DELETE) == 0
               || strcmp(op, CRM_OP_LRM_FAIL) == 0
               || strcmp(op, CRM_OP_LRM_REFRESH) == 0 || strcmp(op, CRM_OP_REPROBE) == 0) {

        crm_xml_add(stored_msg, F_CRM_SYS_TO, CRM_SYSTEM_LRMD);
        return I_ROUTER;

    } else if (strcmp(op, CRM_OP_NOOP) == 0) {
        return I_NULL;

    } else if (strcmp(op, CRM_OP_LOCAL_SHUTDOWN) == 0) {

        crm_shutdown(SIGTERM);
        /*return I_SHUTDOWN; */
        return I_NULL;

    } else if (strcmp(op, CRM_OP_PING) == 0) {
        return handle_ping(stored_msg);

    } else if (strcmp(op, CRM_OP_NODE_INFO) == 0) {
        return handle_node_info_request(stored_msg);

    } else if (strcmp(op, CRM_OP_RM_NODE_CACHE) == 0) {
        int id = 0;
        const char *name = NULL;

        crm_element_value_int(stored_msg, XML_ATTR_ID, &id);
        name = crm_element_value(stored_msg, XML_ATTR_UNAME);

        if(cause == C_IPC_MESSAGE) {
            msg = create_request(CRM_OP_RM_NODE_CACHE, NULL, NULL, CRM_SYSTEM_CRMD, CRM_SYSTEM_CRMD, NULL);
            if (send_cluster_message(NULL, crm_msg_crmd, msg, TRUE) == FALSE) {
                crm_err("Could not instruct peers to remove references to node %s/%u", name, id);
            } else {
                crm_notice("Instructing peers to remove references to node %s/%u", name, id);
            }
            free_xml(msg);

        } else {
            reap_crm_member(id, name);

            /* If we're forgetting this node, also forget any failures to fence
             * it, so we don't carry that over to any node added later with the
             * same name.
             */
            st_fail_count_reset(name);
        }

    } else if (strcmp(op, CRM_OP_MAINTENANCE_NODES) == 0) {
        xmlNode *xml = get_message_xml(stored_msg, F_CRM_DATA);

        remote_ra_process_maintenance_nodes(xml);

        /*========== (NOT_DC)-Only Actions ==========*/
    } else if (AM_I_DC == FALSE && strcmp(op, CRM_OP_SHUTDOWN) == 0) {

        const char *host_from = crm_element_value(stored_msg, F_CRM_HOST_FROM);
        gboolean dc_match = safe_str_eq(host_from, fsa_our_dc);

        if (dc_match || fsa_our_dc == NULL) {
            if (is_set(fsa_input_register, R_SHUTDOWN) == FALSE) {
                crm_err("We didn't ask to be shut down, yet our DC is telling us to.");
                set_bit(fsa_input_register, R_STAYDOWN);
                return I_STOP;
            }
            crm_info("Shutting down");
            return I_STOP;

        } else {
            crm_warn("Discarding %s op from %s", op, host_from);
        }

    } else {
        crm_err("Unexpected request (%s) sent to %s", op, AM_I_DC ? "the DC" : "non-DC node");
        crm_log_xml_err(stored_msg, "Unexpected");
    }

    return I_NULL;
}

void
handle_response(xmlNode * stored_msg)
{
    const char *op = crm_element_value(stored_msg, F_CRM_TASK);

    if (op == NULL) {
        crm_log_xml_err(stored_msg, "Bad message");

    } else if (AM_I_DC && strcmp(op, CRM_OP_PECALC) == 0) {
        // Check whether scheduler answer been superseded by subsequent request
        const char *msg_ref = crm_element_value(stored_msg, XML_ATTR_REFERENCE);

        if (msg_ref == NULL) {
            crm_err("%s - Ignoring calculation with no reference", op);

        } else if (safe_str_eq(msg_ref, fsa_pe_ref)) {
            ha_msg_input_t fsa_input;

            controld_stop_sched_timer();
            fsa_input.msg = stored_msg;
            register_fsa_input_later(C_IPC_MESSAGE, I_PE_SUCCESS, &fsa_input);

        } else {
            crm_info("%s calculation %s is obsolete", op, msg_ref);
        }

    } else if (strcmp(op, CRM_OP_VOTE) == 0
               || strcmp(op, CRM_OP_SHUTDOWN_REQ) == 0 || strcmp(op, CRM_OP_SHUTDOWN) == 0) {

    } else {
        const char *host_from = crm_element_value(stored_msg, F_CRM_HOST_FROM);

        crm_err("Unexpected response (op=%s, src=%s) sent to the %s",
                op, host_from, AM_I_DC ? "DC" : "controller");
    }
}

enum crmd_fsa_input
handle_shutdown_request(xmlNode * stored_msg)
{
    /* handle here to avoid potential version issues
     *   where the shutdown message/procedure may have
     *   been changed in later versions.
     *
     * This way the DC is always in control of the shutdown
     */

    char *now_s = NULL;
    time_t now = time(NULL);
    const char *host_from = crm_element_value(stored_msg, F_CRM_HOST_FROM);

    if (host_from == NULL) {
        /* we're shutting down and the DC */
        host_from = fsa_our_uname;
    }

    crm_info("Creating shutdown request for %s (state=%s)", host_from, fsa_state2string(fsa_state));
    crm_log_xml_trace(stored_msg, "message");

    now_s = crm_itoa(now);
    update_attrd(host_from, XML_CIB_ATTR_SHUTDOWN, now_s, NULL, FALSE);
    free(now_s);

    /* will be picked up by the TE as long as its running */
    return I_NULL;
}

/* msg is deleted by the time this returns */
extern gboolean process_te_message(xmlNode * msg, xmlNode * xml_data);

gboolean
send_msg_via_ipc(xmlNode * msg, const char *sys)
{
    gboolean send_ok = TRUE;
    crm_client_t *client_channel = crm_client_get_by_id(sys);

    if (crm_element_value(msg, F_CRM_HOST_FROM) == NULL) {
        crm_xml_add(msg, F_CRM_HOST_FROM, fsa_our_uname);
    }

    if (client_channel != NULL) {
        /* Transient clients such as crmadmin */
        send_ok = crm_ipcs_send(client_channel, 0, msg, crm_ipc_server_event);

    } else if (sys != NULL && strcmp(sys, CRM_SYSTEM_TENGINE) == 0) {
        xmlNode *data = get_message_xml(msg, F_CRM_DATA);

        process_te_message(msg, data);

    } else if (sys != NULL && strcmp(sys, CRM_SYSTEM_LRMD) == 0) {
        fsa_data_t fsa_data;
        ha_msg_input_t fsa_input;

        fsa_input.msg = msg;
        fsa_input.xml = get_message_xml(msg, F_CRM_DATA);

        fsa_data.id = 0;
        fsa_data.actions = 0;
        fsa_data.data = &fsa_input;
        fsa_data.fsa_input = I_MESSAGE;
        fsa_data.fsa_cause = C_IPC_MESSAGE;
        fsa_data.origin = __FUNCTION__;
        fsa_data.data_type = fsa_dt_ha_msg;

#ifdef FSA_TRACE
        crm_trace("Invoking action A_LRM_INVOKE (%.16llx)", A_LRM_INVOKE);
#endif
        do_lrm_invoke(A_LRM_INVOKE, C_IPC_MESSAGE, fsa_state, I_MESSAGE, &fsa_data);

    } else if (sys != NULL && crmd_is_proxy_session(sys)) {
        crmd_proxy_send(sys, msg);

    } else {
        crm_debug("Unknown Sub-system (%s)... discarding message.", crm_str(sys));
        send_ok = FALSE;
    }

    return send_ok;
}

ha_msg_input_t *
new_ha_msg_input(xmlNode * orig)
{
    ha_msg_input_t *input_copy = NULL;

    input_copy = calloc(1, sizeof(ha_msg_input_t));
    input_copy->msg = orig;
    input_copy->xml = get_message_xml(input_copy->msg, F_CRM_DATA);
    return input_copy;
}

void
delete_ha_msg_input(ha_msg_input_t * orig)
{
    if (orig == NULL) {
        return;
    }
    free_xml(orig->msg);
    free(orig);
}

/*!
 * \internal
 * \brief Notify the DC of a remote node state change
 *
 * \param[in] node_name  Node's name
 * \param[in] node_up    TRUE if node is up, FALSE if down
 */
void
send_remote_state_message(const char *node_name, gboolean node_up)
{
    /* If we don't have a DC, or the message fails, we have a failsafe:
     * the DC will eventually pick up the change via the CIB node state.
     * The message allows it to happen sooner if possible.
     */
    if (fsa_our_dc) {
        xmlNode *msg = create_request(CRM_OP_REMOTE_STATE, NULL, fsa_our_dc,
                                      CRM_SYSTEM_DC, CRM_SYSTEM_CRMD, NULL);

        crm_info("Notifying DC %s of pacemaker_remote node %s %s",
                 fsa_our_dc, node_name, (node_up? "coming up" : "going down"));
        crm_xml_add(msg, XML_ATTR_ID, node_name);
        crm_xml_add_boolean(msg, XML_NODE_IN_CLUSTER, node_up);
        send_cluster_message(crm_get_peer(0, fsa_our_dc), crm_msg_crmd, msg,
                             TRUE);
        free_xml(msg);
    } else {
        crm_debug("No DC to notify of pacemaker_remote node %s %s",
                  node_name, (node_up? "coming up" : "going down"));
    }
}


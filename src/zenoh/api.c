/*
 * Copyright (c) 2017, 2021 ADLINK Technology Inc.
 *
 * This program and the accompanying materials are made available under the
 * terms of the Eclipse Public License 2.0 which is available at
 * http://www.eclipse.org/legal/epl-2.0, or the Apache License, Version 2.0
 * which is available at https://www.apache.org/licenses/LICENSE-2.0.
 *
 * SPDX-License-Identifier: EPL-2.0 OR Apache-2.0
 *
 * Contributors:
 *   ADLINK zenoh team, <zenoh@adlink-labs.tech>
 */

#include "zenoh-pico/protocol/private/utils.h"
#include "zenoh-pico/system/common.h"
#include "zenoh-pico/utils/private/logging.h"
#include "zenoh-pico/session/private/resource.h"
#include "zenoh-pico/session/private/subscription.h"
#include "zenoh-pico/session/private/query.h"
#include "zenoh-pico/session/private/queryable.h"
#include "zenoh-pico/session/private/utils.h"
#include "zenoh-pico/transport/private/utils.h"
#include "zenoh-pico/link/private/result.h"
#include "zenoh-pico/link/private/manager.h"

/*------------------ Init/Config ------------------*/
void z_init_logger()
{
    // @TODO
}

zn_properties_t *zn_config_empty()
{
    return zn_properties_make();
}

zn_properties_t *zn_config_client(const char *locator)
{
    zn_properties_t *ps = zn_config_empty();
    zn_properties_insert(ps, ZN_CONFIG_MODE_KEY, z_string_make("client"));
    if (locator)
    {
        // Connect only to the provided locator
        zn_properties_insert(ps, ZN_CONFIG_PEER_KEY, z_string_make(locator));
    }
    else
    {
        // The locator is not provided, we should perform scouting
        zn_properties_insert(ps, ZN_CONFIG_MULTICAST_SCOUTING_KEY, z_string_make(ZN_CONFIG_MULTICAST_SCOUTING_DEFAULT));
        zn_properties_insert(ps, ZN_CONFIG_MULTICAST_ADDRESS_KEY, z_string_make(ZN_CONFIG_MULTICAST_ADDRESS_DEFAULT));
        zn_properties_insert(ps, ZN_CONFIG_MULTICAST_INTERFACE_KEY, z_string_make(ZN_CONFIG_MULTICAST_INTERFACE_DEFAULT));
        zn_properties_insert(ps, ZN_CONFIG_SCOUTING_TIMEOUT_KEY, z_string_make(ZN_CONFIG_SCOUTING_TIMEOUT_DEFAULT));
    }
    return ps;
}

zn_properties_t *zn_config_default()
{
    return zn_config_client(NULL);
}

/*------------------ Scout/Open/Close ------------------*/
void zn_hello_array_free(zn_hello_array_t hellos)
{
    zn_hello_t *h = (zn_hello_t *)hellos.val;
    if (h)
    {
        for (unsigned int i = 0; i < hellos.len; i++)
        {
            if (h[i].pid.len > 0)
                _z_bytes_free(&h[i].pid);
            if (h[i].locators.len > 0)
                _z_str_array_free(&h[i].locators);
        }

        free(h);
    }
}

zn_hello_array_t zn_scout(unsigned int what, zn_properties_t *config, unsigned long timeout)
{
    return _zn_scout(what, config, timeout, 0);
}

void zn_close(zn_session_t *zn)
{
    _zn_session_close(zn, _ZN_CLOSE_GENERIC);
    return;
}

zn_session_t *zn_open(zn_properties_t *config)
{
    zn_session_t *zn = NULL;

    int locator_is_scouted = 0;
    const char *locator = zn_properties_get(config, ZN_CONFIG_PEER_KEY).val;

    if (locator == NULL)
    {
        // Scout for routers
        unsigned int what = ZN_ROUTER;
        const char *mode = zn_properties_get(config, ZN_CONFIG_MODE_KEY).val;
        if (mode == NULL)
        {
            return zn;
        }

        // The ZN_CONFIG_SCOUTING_TIMEOUT_KEY is expressed in seconds as a float while the
        // scout loop timeout uses milliseconds granularity
        const char *to = zn_properties_get(config, ZN_CONFIG_SCOUTING_TIMEOUT_KEY).val;
        if (to == NULL)
        {
            to = ZN_CONFIG_SCOUTING_TIMEOUT_DEFAULT;
        }
        clock_t timeout = (clock_t)1000 * strtof(to, NULL);

        // Scout and return upon the first result
        zn_hello_array_t locs = _zn_scout(what, config, timeout, 1);
        if (locs.len > 0)
        {
            if (locs.val[0].locators.len > 0)
            {
                locator = strdup(locs.val[0].locators.val[0]);
                // Mark that the locator has been scouted, need to be freed before returning
                locator_is_scouted = 1;
            }
            // Free all the scouted locators
            zn_hello_array_free(locs);
        }
        else
        {
            _Z_DEBUG("Unable to scout a zenoh router\n");
            _Z_ERROR("%sPlease make sure one is running on your network!\n", "");
            // Free all the scouted locators
            zn_hello_array_free(locs);

            return zn;
        }
    }

    // Initialize the PRNG
    srand(time(NULL));

    // Attempt to configure the link
    _zn_link_p_result_t r_link = _zn_open_link(locator, 0);
    if (r_link.tag == _z_res_t_ERR)
    {
        if (locator_is_scouted)
            free((char *)locator);

        return zn;
    }

    // Randomly generate a peer ID
    z_bytes_t pid = _z_bytes_make(ZN_PID_LENGTH);
    for (unsigned int i = 0; i < pid.len; i++)
        ((uint8_t *)pid.val)[i] = rand() % 255;

    // Build the open message
    _zn_transport_message_t ism = _zn_transport_message_init(_ZN_MID_INIT);

    ism.body.init.options = 0;
    ism.body.init.version = ZN_PROTO_VERSION;
    ism.body.init.whatami = ZN_CLIENT;
    ism.body.init.pid = pid;
    ism.body.init.sn_resolution = ZN_SN_RESOLUTION;

    if (ZN_SN_RESOLUTION != ZN_SN_RESOLUTION_DEFAULT)
        _ZN_SET_FLAG(ism.header, _ZN_FLAG_T_S);

    // Initialize the session
    zn = _zn_session_init();
    zn->link = r_link.value.link;

    _Z_DEBUG("Sending InitSyn\n");
    // Encode and send the message
    int res = _zn_send_t_msg(zn, &ism);
    if (res != 0)
    {
        // Free the pid
        _z_bytes_free(&pid);

        // Free the locator
        if (locator_is_scouted)
            free((char *)locator);

        // Free
        _zn_transport_message_free(&ism);
        _zn_session_free(zn);

        return zn;
    }

    _zn_transport_message_p_result_t r_msg = _zn_recv_t_msg(zn);
    if (r_msg.tag == _z_res_t_ERR)
    {
        // Free the pid
        _z_bytes_free(&pid);

        // Free the locator
        if (locator_is_scouted)
            free((char *)locator);

        // Free
        _zn_transport_message_free(&ism);
        _zn_session_free(zn);

        return zn;
    }

    _zn_transport_message_t *p_iam = r_msg.value.transport_message;
    switch (_ZN_MID(p_iam->header))
    {
    case _ZN_MID_INIT:
    {
        if _ZN_HAS_FLAG (p_iam->header, _ZN_FLAG_T_A)
        {
            // The announced sn resolution
            zn->sn_resolution = ism.body.init.sn_resolution;
            zn->sn_resolution_half = zn->sn_resolution / 2;

            // Handle SN resolution option if present
            if _ZN_HAS_FLAG (p_iam->header, _ZN_FLAG_T_S)
            {
                // The resolution in the InitAck must be less or equal than the resolution in the InitSyn,
                // otherwise the InitAck message is considered invalid and it should be treated as a
                // CLOSE message with L==0 by the Initiating Peer -- the recipient of the InitAck message.
                if (p_iam->body.init.sn_resolution <= ism.body.init.sn_resolution)
                {
                    zn->sn_resolution = p_iam->body.init.sn_resolution;
                    zn->sn_resolution_half = zn->sn_resolution / 2;
                }
                else
                {
                    // Close the session
                    _zn_session_close(zn, _ZN_CLOSE_INVALID);
                    break;
                }
            }

            // The initial SN at TX side
            z_zint_t initial_sn = (z_zint_t)rand() % zn->sn_resolution;
            zn->sn_tx_reliable = initial_sn;
            zn->sn_tx_best_effort = initial_sn;

            // Create the OpenSyn message
            _zn_transport_message_t osm = _zn_transport_message_init(_ZN_MID_OPEN);
            osm.body.open.lease = ZN_TRANSPORT_LEASE;
            if (ZN_TRANSPORT_LEASE % 1000 == 0)
                _ZN_SET_FLAG(osm.header, _ZN_FLAG_T_T2);
            osm.body.open.initial_sn = initial_sn;
            osm.body.open.cookie = p_iam->body.init.cookie;

            _Z_DEBUG("Sending OpenSyn\n");
            // Encode and send the message
            int res = _zn_send_t_msg(zn, &osm);
            _zn_transport_message_free(&osm);
            if (res != 0)
            {
                _z_bytes_free(&pid);
                if (locator_is_scouted)
                    free((char *)locator);
                _zn_session_free(zn);
                break;
            }

            // Initialize the Local and Remote Peer IDs
            _z_bytes_move(&zn->local_pid, &pid);
            _z_bytes_copy(&zn->remote_pid, &p_iam->body.init.pid);

            if (locator_is_scouted)
                zn->locator = (char *)locator;
            else
                zn->locator = strdup(locator);

            break;
        }
        else
        {
            // Close the session
            _zn_session_close(zn, _ZN_CLOSE_INVALID);
            break;
        }
    }

    default:
    {
        // Close the session
        _zn_session_close(zn, _ZN_CLOSE_INVALID);
        break;
    }
    }

    // Free the messages and result
    _zn_transport_message_free(&ism);
    _zn_transport_message_free(p_iam);
    _zn_transport_message_p_result_free(&r_msg);

    return zn;
}

zn_properties_t *zn_info(zn_session_t *zn)
{
    zn_properties_t *ps = zn_properties_make();
    zn_properties_insert(ps, ZN_INFO_PID_KEY, _z_string_from_bytes(&zn->local_pid));
    zn_properties_insert(ps, ZN_INFO_ROUTER_PID_KEY, _z_string_from_bytes(&zn->remote_pid));
    return ps;
}

void zn_sample_free(zn_sample_t sample)
{
    if (sample.key.val)
        _z_string_free(&sample.key);
    if (sample.value.val)
        _z_bytes_free(&sample.value);
}

/*------------------ Resource Keys operations ------------------*/
zn_reskey_t zn_rname(const char *rname)
{
    zn_reskey_t rk;
    rk.rid = ZN_RESOURCE_ID_NONE;
    rk.rname = strdup(rname);
    return rk;
}

zn_reskey_t zn_rid(unsigned long rid)
{
    zn_reskey_t rk;
    rk.rid = rid;
    rk.rname = NULL;
    return rk;
}

zn_reskey_t zn_rid_with_suffix(unsigned long id, const char *suffix)
{
    zn_reskey_t rk;
    rk.rid = id;
    rk.rname = strdup(suffix);
    return rk;
}

/*------------------ Resource Declaration ------------------*/
z_zint_t zn_declare_resource(zn_session_t *zn, zn_reskey_t reskey)
{
    _zn_resource_t *r = (_zn_resource_t *)malloc(sizeof(_zn_resource_t));
    r->id = _zn_get_resource_id(zn);
    r->key = reskey;

    int res = _zn_register_resource(zn, _ZN_IS_LOCAL, r);
    if (res != 0)
    {
        free(r);
        return ZN_RESOURCE_ID_NONE;
    }

    // Build the declare message to send on the wire
    _zn_zenoh_message_t z_msg = _zn_zenoh_message_init(_ZN_MID_DECLARE);

    // We need to declare the resource
    unsigned int len = 1;
    z_msg.body.declare.declarations.len = len;
    z_msg.body.declare.declarations.val = (_zn_declaration_t *)malloc(len * sizeof(_zn_declaration_t));

    // Resource declaration
    z_msg.body.declare.declarations.val[0].header = _ZN_DECL_RESOURCE;
    z_msg.body.declare.declarations.val[0].body.res.id = r->id;
    z_msg.body.declare.declarations.val[0].body.res.key = _zn_reskey_clone(&r->key);
    if (r->key.rname)
        _ZN_SET_FLAG(z_msg.body.declare.declarations.val[0].header, _ZN_FLAG_Z_K);

    if (_zn_send_z_msg(zn, &z_msg, zn_reliability_t_RELIABLE, zn_congestion_control_t_BLOCK) != 0)
    {
        _Z_DEBUG("Trying to reconnect...\n");
        zn->on_disconnect(zn);
        _zn_send_z_msg(zn, &z_msg, zn_reliability_t_RELIABLE, zn_congestion_control_t_BLOCK);
    }

    _zn_zenoh_message_free(&z_msg);

    return r->id;
}

void zn_undeclare_resource(zn_session_t *zn, z_zint_t rid)
{
    _zn_resource_t *r = _zn_get_resource_by_id(zn, _ZN_IS_LOCAL, rid);
    if (r)
    {
        _zn_zenoh_message_t z_msg = _zn_zenoh_message_init(_ZN_MID_DECLARE);

        // We need to undeclare the resource and the publisher
        unsigned int len = 1;
        z_msg.body.declare.declarations.len = len;
        z_msg.body.declare.declarations.val = (_zn_declaration_t *)malloc(len * sizeof(_zn_declaration_t));

        // Resource declaration
        z_msg.body.declare.declarations.val[0].header = _ZN_DECL_FORGET_RESOURCE;
        z_msg.body.declare.declarations.val[0].body.forget_res.rid = rid;

        if (_zn_send_z_msg(zn, &z_msg, zn_reliability_t_RELIABLE, zn_congestion_control_t_BLOCK) != 0)
        {
            _Z_DEBUG("Trying to reconnect...\n");
            zn->on_disconnect(zn);
            _zn_send_z_msg(zn, &z_msg, zn_reliability_t_RELIABLE, zn_congestion_control_t_BLOCK);
        }

        _zn_zenoh_message_free(&z_msg);

        _zn_unregister_resource(zn, _ZN_IS_LOCAL, r);
    }
}

/*------------------  Publisher Declaration ------------------*/
zn_publisher_t *zn_declare_publisher(zn_session_t *zn, zn_reskey_t reskey)
{
    zn_publisher_t *pub = (zn_publisher_t *)malloc(sizeof(zn_publisher_t));
    pub->zn = zn;
    pub->key = reskey;
    pub->id = _zn_get_entity_id(zn);

    _zn_zenoh_message_t z_msg = _zn_zenoh_message_init(_ZN_MID_DECLARE);

    // We need to declare the resource and the publisher
    unsigned int len = 1;
    z_msg.body.declare.declarations.len = len;
    z_msg.body.declare.declarations.val = (_zn_declaration_t *)malloc(len * sizeof(_zn_declaration_t));

    // Publisher declaration
    z_msg.body.declare.declarations.val[0].header = _ZN_DECL_PUBLISHER;
    z_msg.body.declare.declarations.val[0].body.pub.key = _zn_reskey_clone(&reskey);
    ;
    // Mark the key as string if the key has resource name
    if (pub->key.rname)
        _ZN_SET_FLAG(z_msg.body.declare.declarations.val[0].header, _ZN_FLAG_Z_K);

    if (_zn_send_z_msg(zn, &z_msg, zn_reliability_t_RELIABLE, zn_congestion_control_t_BLOCK) != 0)
    {
        _Z_DEBUG("Trying to reconnect...\n");
        zn->on_disconnect(zn);
        _zn_send_z_msg(zn, &z_msg, zn_reliability_t_RELIABLE, zn_congestion_control_t_BLOCK);
    }

    _zn_zenoh_message_free(&z_msg);

    return pub;
}

void zn_undeclare_publisher(zn_publisher_t *pub)
{
    _zn_zenoh_message_t z_msg = _zn_zenoh_message_init(_ZN_MID_DECLARE);

    // We need to undeclare the publisher
    unsigned int len = 1;
    z_msg.body.declare.declarations.len = len;
    z_msg.body.declare.declarations.val = (_zn_declaration_t *)malloc(len * sizeof(_zn_declaration_t));

    // Forget publisher declaration
    z_msg.body.declare.declarations.val[0].header = _ZN_DECL_FORGET_PUBLISHER;
    z_msg.body.declare.declarations.val[0].body.forget_pub.key = _zn_reskey_clone(&pub->key);
    // Mark the key as string if the key has resource name
    if (pub->key.rname)
        _ZN_SET_FLAG(z_msg.body.declare.declarations.val[0].header, _ZN_FLAG_Z_K);

    if (_zn_send_z_msg(pub->zn, &z_msg, zn_reliability_t_RELIABLE, zn_congestion_control_t_BLOCK) != 0)
    {
        _Z_DEBUG("Trying to reconnect...\n");
        pub->zn->on_disconnect(pub->zn);
        _zn_send_z_msg(pub->zn, &z_msg, zn_reliability_t_RELIABLE, zn_congestion_control_t_BLOCK);
    }

    _zn_zenoh_message_free(&z_msg);

    free(pub);
}

/*------------------ Subscriber Declaration ------------------*/
zn_subinfo_t zn_subinfo_default()
{
    zn_subinfo_t si;
    si.reliability = zn_reliability_t_RELIABLE;
    si.mode = zn_submode_t_PUSH;
    si.period = NULL;
    return si;
}

zn_subscriber_t *zn_declare_subscriber(zn_session_t *zn, zn_reskey_t reskey, zn_subinfo_t sub_info, zn_data_handler_t callback, void *arg)
{
    _zn_subscriber_t *rs = (_zn_subscriber_t *)malloc(sizeof(_zn_subscriber_t));
    rs->id = _zn_get_entity_id(zn);
    rs->key = reskey;
    rs->info = sub_info;
    rs->callback = callback;
    rs->arg = arg;

    int res = _zn_register_subscription(zn, _ZN_IS_LOCAL, rs);
    if (res != 0)
    {
        free(rs);
        return NULL;
    }

    _zn_zenoh_message_t z_msg = _zn_zenoh_message_init(_ZN_MID_DECLARE);

    // We need to declare the subscriber
    unsigned int len = 1;
    z_msg.body.declare.declarations.len = len;
    z_msg.body.declare.declarations.val = (_zn_declaration_t *)malloc(len * sizeof(_zn_declaration_t));

    // Subscriber declaration
    z_msg.body.declare.declarations.val[0].header = _ZN_DECL_SUBSCRIBER;
    if (reskey.rname)
        _ZN_SET_FLAG(z_msg.body.declare.declarations.val[0].header, _ZN_FLAG_Z_K);
    if (sub_info.mode != zn_submode_t_PUSH || sub_info.period)
        _ZN_SET_FLAG(z_msg.body.declare.declarations.val[0].header, _ZN_FLAG_Z_S);
    if (sub_info.reliability == zn_reliability_t_RELIABLE)
        _ZN_SET_FLAG(z_msg.body.declare.declarations.val[0].header, _ZN_FLAG_Z_R);

    z_msg.body.declare.declarations.val[0].body.sub.key = _zn_reskey_clone(&reskey);

    // SubMode
    z_msg.body.declare.declarations.val[0].body.sub.subinfo.mode = sub_info.mode;
    z_msg.body.declare.declarations.val[0].body.sub.subinfo.reliability = sub_info.reliability;
    z_msg.body.declare.declarations.val[0].body.sub.subinfo.period = sub_info.period;

    if (_zn_send_z_msg(zn, &z_msg, zn_reliability_t_RELIABLE, zn_congestion_control_t_BLOCK) != 0)
    {
        _Z_DEBUG("Trying to reconnect....\n");
        zn->on_disconnect(zn);
        _zn_send_z_msg(zn, &z_msg, zn_reliability_t_RELIABLE, zn_congestion_control_t_BLOCK);
    }

    _zn_zenoh_message_free(&z_msg);

    zn_subscriber_t *subscriber = (zn_subscriber_t *)malloc(sizeof(zn_subscriber_t));
    subscriber->zn = zn;
    subscriber->id = rs->id;

    return subscriber;
}

void zn_undeclare_subscriber(zn_subscriber_t *sub)
{
    _zn_subscriber_t *s = _zn_get_subscription_by_id(sub->zn, _ZN_IS_LOCAL, sub->id);
    if (s)
    {
        _zn_zenoh_message_t z_msg = _zn_zenoh_message_init(_ZN_MID_DECLARE);

        // We need to undeclare the subscriber
        unsigned int len = 1;
        z_msg.body.declare.declarations.len = len;
        z_msg.body.declare.declarations.val = (_zn_declaration_t *)malloc(len * sizeof(_zn_declaration_t));

        // Forget Subscriber declaration
        z_msg.body.declare.declarations.val[0].header = _ZN_DECL_FORGET_SUBSCRIBER;
        if (s->key.rname)
            _ZN_SET_FLAG(z_msg.body.declare.declarations.val[0].header, _ZN_FLAG_Z_K);

        z_msg.body.declare.declarations.val[0].body.forget_sub.key = _zn_reskey_clone(&s->key);

        if (_zn_send_z_msg(sub->zn, &z_msg, zn_reliability_t_RELIABLE, zn_congestion_control_t_BLOCK) != 0)
        {
            _Z_DEBUG("Trying to reconnect....\n");
            sub->zn->on_disconnect(sub->zn);
            _zn_send_z_msg(sub->zn, &z_msg, zn_reliability_t_RELIABLE, zn_congestion_control_t_BLOCK);
        }

        _zn_zenoh_message_free(&z_msg);

        _zn_unregister_subscription(sub->zn, _ZN_IS_LOCAL, s);
    }

    free(sub);
}

/*------------------ Write ------------------*/
int zn_write_ext(zn_session_t *zn, zn_reskey_t reskey, const unsigned char *payload, size_t length, uint8_t encoding, uint8_t kind, zn_congestion_control_t cong_ctrl)
{
    // @TODO: Need to verify that I have declared a publisher with the same resource key.
    //        Then, need to verify there are active subscriptions matching the publisher.
    // @TODO: Need to check subscriptions to determine the right reliability value.

    _zn_zenoh_message_t z_msg = _zn_zenoh_message_init(_ZN_MID_DATA);
    // Eventually mark the message for congestion control
    if (cong_ctrl == zn_congestion_control_t_DROP)
        _ZN_SET_FLAG(z_msg.header, _ZN_FLAG_Z_D);
    // Set the resource key
    z_msg.body.data.key = reskey;
    _ZN_SET_FLAG(z_msg.header, reskey.rname ? _ZN_FLAG_Z_K : 0);

    // Set the data info
    _ZN_SET_FLAG(z_msg.header, _ZN_FLAG_Z_I);
    _zn_data_info_t info;
    info.flags = 0;
    info.encoding.prefix = encoding;
    info.encoding.suffix = ""; // TODO: empty for now, but expose this in the function signature
    _ZN_SET_FLAG(info.flags, _ZN_DATA_INFO_ENC);
    info.kind = kind;
    _ZN_SET_FLAG(info.flags, _ZN_DATA_INFO_KIND);
    z_msg.body.data.info = info;

    // Set the payload
    z_msg.body.data.payload.len = length;
    z_msg.body.data.payload.val = (uint8_t *)payload;

    return _zn_send_z_msg(zn, &z_msg, zn_reliability_t_RELIABLE, cong_ctrl);
}

int zn_write(zn_session_t *zn, zn_reskey_t reskey, const uint8_t *payload, size_t length)
{
    // @TODO: Need to verify that I have declared a publisher with the same resource key.
    //        Then, need to verify there are active subscriptions matching the publisher.
    // @TODO: Need to check subscriptions to determine the right reliability value.

    _zn_zenoh_message_t z_msg = _zn_zenoh_message_init(_ZN_MID_DATA);
    // Eventually mark the message for congestion control
    if (ZN_CONGESTION_CONTROL_DEFAULT == zn_congestion_control_t_DROP)
        _ZN_SET_FLAG(z_msg.header, _ZN_FLAG_Z_D);
    // Set the resource key
    z_msg.body.data.key = reskey;
    _ZN_SET_FLAG(z_msg.header, reskey.rname ? _ZN_FLAG_Z_K : 0);

    // Set the payload
    z_msg.body.data.payload.len = length;
    z_msg.body.data.payload.val = (uint8_t *)payload;

    return _zn_send_z_msg(zn, &z_msg, zn_reliability_t_RELIABLE, ZN_CONGESTION_CONTROL_DEFAULT);
}

/*------------------ Query/Queryable ------------------*/
zn_query_consolidation_t zn_query_consolidation_default(void)
{
    zn_query_consolidation_t qc;
    qc.first_routers = zn_consolidation_mode_t_LAZY;
    qc.last_router = zn_consolidation_mode_t_LAZY;
    qc.reception = zn_consolidation_mode_t_FULL;
    return qc;
}

zn_query_consolidation_t zn_query_consolidation_none(void)
{
    zn_query_consolidation_t qc;
    qc.first_routers = zn_consolidation_mode_t_NONE;
    qc.last_router = zn_consolidation_mode_t_NONE;
    qc.reception = zn_consolidation_mode_t_NONE;
    return qc;
}

z_string_t zn_query_predicate(zn_query_t *query)
{
    z_string_t s;
    s.len = strlen(query->predicate);
    s.val = query->predicate;
    return s;
}

z_string_t zn_query_res_name(zn_query_t *query)
{
    z_string_t s;
    s.len = strlen(query->rname);
    s.val = query->rname;
    return s;
}

zn_target_t zn_target_default(void)
{
    zn_target_t t;
    t.tag = zn_target_t_BEST_MATCHING;
    return t;
}

zn_query_target_t zn_query_target_default(void)
{
    zn_query_target_t qt;
    qt.kind = ZN_QUERYABLE_ALL_KINDS;
    qt.target = zn_target_default();
    return qt;
}

int zn_query_target_equal(zn_query_target_t *left, zn_query_target_t *right)
{
    return memcmp(left, right, sizeof(zn_query_target_t));
}

int zn_query_consolidation_equal(zn_query_consolidation_t *left, zn_query_consolidation_t *right)
{
    return memcmp(left, right, sizeof(zn_query_consolidation_t));
}

void zn_query(zn_session_t *zn, zn_reskey_t reskey, const char *predicate, zn_query_target_t target, zn_query_consolidation_t consolidation, zn_query_handler_t callback, void *arg)
{
    // Create the pending query object
    _zn_pending_query_t *pq = (_zn_pending_query_t *)malloc(sizeof(_zn_pending_query_t));
    pq->id = _zn_get_query_id(zn);
    pq->key = reskey;
    pq->predicate = strdup(predicate);
    pq->target = target;
    pq->consolidation = consolidation;
    pq->callback = callback;
    pq->pending_replies = z_list_empty;
    pq->arg = arg;

    // Add the pending query to the current session
    _zn_register_pending_query(zn, pq);

    // Send the query
    _zn_zenoh_message_t z_msg = _zn_zenoh_message_init(_ZN_MID_QUERY);
    z_msg.body.query.qid = pq->id;
    z_msg.body.query.key = reskey;
    _ZN_SET_FLAG(z_msg.header, reskey.rname ? _ZN_FLAG_Z_K : 0);
    z_msg.body.query.predicate = (z_str_t)predicate;

    zn_query_target_t qtd = zn_query_target_default();
    if (!zn_query_target_equal(&target, &qtd))
    {
        _ZN_SET_FLAG(z_msg.header, _ZN_FLAG_Z_T);
        z_msg.body.query.target = target;
    }

    z_msg.body.query.consolidation = consolidation;

    int res = _zn_send_z_msg(zn, &z_msg, zn_reliability_t_RELIABLE, zn_congestion_control_t_BLOCK);
    if (res != 0)
        _zn_unregister_pending_query(zn, pq);
}

void reply_collect_handler(const zn_reply_t reply, const void *arg)
{
    _zn_pending_query_collect_t *pqc = (_zn_pending_query_collect_t *)arg;
    if (reply.tag == zn_reply_t_Tag_DATA)
    {
        zn_reply_data_t *rd = (zn_reply_data_t *)malloc(sizeof(zn_reply_data_t));
        rd->replier_kind = reply.data.replier_kind;
        _z_bytes_copy(&rd->replier_id, &reply.data.replier_id);
        _z_string_copy(&rd->data.key, &reply.data.data.key);
        _z_bytes_copy(&rd->data.value, &reply.data.data.value);

        z_vec_append(&pqc->replies, rd);
    }
    else
    {
        // Signal that we have received all the replies
        z_condvar_signal(&pqc->cond_var);
    }
}

zn_reply_data_array_t zn_query_collect(zn_session_t *zn,
                                       zn_reskey_t reskey,
                                       const char *predicate,
                                       zn_query_target_t target,
                                       zn_query_consolidation_t consolidation)
{
    // Create the synchronization variables
    _zn_pending_query_collect_t pqc;
    z_mutex_init(&pqc.mutex);
    z_condvar_init(&pqc.cond_var);
    pqc.replies = z_vec_make(1);

    // Issue the query
    zn_query(zn, reskey, predicate, target, consolidation, reply_collect_handler, &pqc);

    // Wait to be notified
    z_mutex_lock(&pqc.mutex);
    z_condvar_wait(&pqc.cond_var, &pqc.mutex);

    zn_reply_data_array_t rda;
    rda.len = z_vec_len(&pqc.replies);
    zn_reply_data_t *replies = (zn_reply_data_t *)malloc(rda.len * sizeof(zn_reply_data_t));
    for (unsigned int i = 0; i < rda.len; i++)
    {
        zn_reply_data_t *reply = (zn_reply_data_t *)z_vec_get(&pqc.replies, i);
        replies[i].replier_kind = reply->replier_kind;
        _z_bytes_move(&replies[i].replier_id, &reply->replier_id);
        _z_string_move(&replies[i].data.key, &reply->data.key);
        _z_bytes_move(&replies[i].data.value, &reply->data.value);
    }
    rda.val = replies;

    z_vec_free(&pqc.replies);
    z_condvar_free(&pqc.cond_var);
    z_mutex_free(&pqc.mutex);

    return rda;
}

void zn_reply_data_array_free(zn_reply_data_array_t replies)
{
    for (unsigned int i = 0; i < replies.len; i++)
    {
        if (replies.val[i].replier_id.val)
            _z_bytes_free((z_bytes_t *)&replies.val[i].replier_id);
        if (replies.val[i].data.value.val)
            _z_bytes_free((z_bytes_t *)&replies.val[i].data.value);
        if (replies.val[i].data.key.val)
            _z_string_free((z_string_t *)&replies.val[i].data.key);
    }
    free((zn_reply_data_t *)replies.val);
}

zn_queryable_t *zn_declare_queryable(zn_session_t *zn, zn_reskey_t reskey, unsigned int kind, zn_queryable_handler_t callback, void *arg)
{
    _zn_queryable_t *rq = (_zn_queryable_t *)malloc(sizeof(_zn_queryable_t));
    rq->id = _zn_get_entity_id(zn);
    rq->key = reskey;
    rq->kind = kind;
    rq->callback = callback;
    rq->arg = arg;

    int res = _zn_register_queryable(zn, rq);
    if (res != 0)
    {
        free(rq);
        return NULL;
    }

    _zn_zenoh_message_t z_msg = _zn_zenoh_message_init(_ZN_MID_DECLARE);

    // We need to declare the queryable
    unsigned int len = 1;
    z_msg.body.declare.declarations.len = len;
    z_msg.body.declare.declarations.val = (_zn_declaration_t *)malloc(len * sizeof(_zn_declaration_t));

    // Queryable declaration
    z_msg.body.declare.declarations.val[0].header = _ZN_DECL_QUERYABLE;
    if (reskey.rname)
        _ZN_SET_FLAG(z_msg.body.declare.declarations.val[0].header, _ZN_FLAG_Z_K);

    z_zint_t complete = _ZN_QUERYABLE_COMPLETE_DEFAULT;
    z_zint_t distance = _ZN_QUERYABLE_DISTANCE_DEFAULT;
    if (complete != _ZN_QUERYABLE_COMPLETE_DEFAULT || distance != _ZN_QUERYABLE_DISTANCE_DEFAULT)
        _ZN_SET_FLAG(z_msg.body.declare.declarations.val[0].header, _ZN_FLAG_Z_Q);

    z_msg.body.declare.declarations.val[0]
        .body.qle.key = _zn_reskey_clone(&reskey);
    z_msg.body.declare.declarations.val[0]
        .body.qle.kind = (z_zint_t)kind;
    z_msg.body.declare.declarations.val[0]
        .body.qle.complete = complete;
    z_msg.body.declare.declarations.val[0]
        .body.qle.distance = distance;

    if (_zn_send_z_msg(zn, &z_msg, zn_reliability_t_RELIABLE, zn_congestion_control_t_BLOCK) != 0)
    {
        _Z_DEBUG("Trying to reconnect....\n");
        zn->on_disconnect(zn);
        _zn_send_z_msg(zn, &z_msg, zn_reliability_t_RELIABLE, zn_congestion_control_t_BLOCK);
    }

    _zn_zenoh_message_free(&z_msg);

    zn_queryable_t *queryable = (zn_queryable_t *)malloc(sizeof(zn_queryable_t));
    queryable->zn = zn;
    queryable->id = rq->id;

    return queryable;
}

void zn_undeclare_queryable(zn_queryable_t *qle)
{
    _zn_queryable_t *q = _zn_get_queryable_by_id(qle->zn, qle->id);
    if (q)
    {
        _zn_zenoh_message_t z_msg = _zn_zenoh_message_init(_ZN_MID_DECLARE);

        // We need to undeclare the queryable
        unsigned int len = 1;
        z_msg.body.declare.declarations.len = len;
        z_msg.body.declare.declarations.val = (_zn_declaration_t *)malloc(len * sizeof(_zn_declaration_t));

        // Forget Queryable declaration
        z_msg.body.declare.declarations.val[0].header = _ZN_DECL_FORGET_QUERYABLE;
        if (q->key.rname)
            _ZN_SET_FLAG(z_msg.body.declare.declarations.val[0].header, _ZN_FLAG_Z_K);

        z_msg.body.declare.declarations.val[0].body.forget_qle.key = _zn_reskey_clone(&q->key);
        z_msg.body.declare.declarations.val[0].body.forget_qle.kind = q->kind;

        if (_zn_send_z_msg(qle->zn, &z_msg, zn_reliability_t_RELIABLE, zn_congestion_control_t_BLOCK) != 0)
        {
            _Z_DEBUG("Trying to reconnect....\n");
            qle->zn->on_disconnect(qle->zn);
            _zn_send_z_msg(qle->zn, &z_msg, zn_reliability_t_RELIABLE, zn_congestion_control_t_BLOCK);
        }

        _zn_zenoh_message_free(&z_msg);

        _zn_unregister_queryable(qle->zn, q);
    }

    free(qle);
}

void zn_send_reply(zn_query_t *query, const char *key, const uint8_t *payload, size_t len)
{
    _zn_zenoh_message_t z_msg = _zn_zenoh_message_init(_ZN_MID_DATA);

    // Build the reply context decorator. This is NOT the final reply.
    z_msg.reply_context = _zn_reply_context_init();
    z_msg.reply_context->qid = query->qid;
    z_msg.reply_context->replier_kind = query->kind;
    z_msg.reply_context->replier_id = query->zn->local_pid;

    // Build the data payload
    z_msg.body.data.payload.val = payload;
    z_msg.body.data.payload.len = len;
    // @TODO: use numerical resources if possible
    z_msg.body.data.key.rid = ZN_RESOURCE_ID_NONE;
    z_msg.body.data.key.rname = (z_str_t)key;
    if (z_msg.body.data.key.rname)
        _ZN_SET_FLAG(z_msg.header, _ZN_FLAG_Z_K);
    // Do not set any data_info

    if (_zn_send_z_msg(query->zn, &z_msg, zn_reliability_t_RELIABLE, zn_congestion_control_t_BLOCK) != 0)
    {
        _Z_DEBUG("Trying to reconnect....\n");
        query->zn->on_disconnect(query->zn);
        _zn_send_z_msg(query->zn, &z_msg, zn_reliability_t_RELIABLE, zn_congestion_control_t_BLOCK);
    }

    free(z_msg.reply_context);
}

/*------------------ Pull ------------------*/
int zn_pull(zn_subscriber_t *sub)
{
    _zn_subscriber_t *s = _zn_get_subscription_by_id(sub->zn, _ZN_IS_LOCAL, sub->id);
    if (s)
    {
        _zn_zenoh_message_t z_msg = _zn_zenoh_message_init(_ZN_MID_PULL);
        z_msg.body.pull.key = _zn_reskey_clone(&s->key);
        _ZN_SET_FLAG(z_msg.header, s->key.rname ? _ZN_FLAG_Z_K : 0);
        _ZN_SET_FLAG(z_msg.header, _ZN_FLAG_Z_F);

        z_msg.body.pull.pull_id = _zn_get_pull_id(sub->zn);
        // @TODO: get the correct value for max_sample
        z_msg.body.pull.max_samples = 0;
        // _ZN_SET_FLAG(z_msg.header, _ZN_FLAG_Z_N);

        if (_zn_send_z_msg(sub->zn, &z_msg, zn_reliability_t_RELIABLE, zn_congestion_control_t_BLOCK) != 0)
        {
            _Z_DEBUG("Trying to reconnect....\n");
            sub->zn->on_disconnect(sub->zn);
            _zn_send_z_msg(sub->zn, &z_msg, zn_reliability_t_RELIABLE, zn_congestion_control_t_BLOCK);
        }

        _zn_zenoh_message_free(&z_msg);

        return 0;
    }
    else
    {
        return -1;
    }
}

/*-----------------------------------------------------------*/
/*------------------ Zenoh-pico operations ------------------*/
/*-----------------------------------------------------------*/
/*------------------ Read ------------------*/
int znp_read(zn_session_t *zn)
{
    _zn_transport_message_p_result_t r_s = _zn_recv_t_msg(zn);
    if (r_s.tag == _z_res_t_OK)
    {
        int res = _zn_handle_transport_message(zn, r_s.value.transport_message);
        _zn_transport_message_free(r_s.value.transport_message);
        _zn_transport_message_p_result_free(&r_s);
        return res;
    }
    else
    {
        _zn_transport_message_p_result_free(&r_s);
        return _z_res_t_ERR;
    }
}

/*------------------ Keep Alive ------------------*/
int znp_send_keep_alive(zn_session_t *zn)
{
    _zn_transport_message_t t_msg = _zn_transport_message_init(_ZN_MID_KEEP_ALIVE);

    return _zn_send_t_msg(zn, &t_msg);
}

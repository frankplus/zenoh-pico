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
#ifndef _ZENOH_PICO_SESSION_PRIVATE_TYPES_H
#define _ZENOH_PICO_SESSION_PRIVATE_TYPES_H

#include "zenoh-pico/protocol/types.h"
#include "zenoh-pico/session/types.h"
#include "zenoh-pico/utils/collections.h"
#include "zenoh-pico/utils/types.h"

#define _ZN_IS_REMOTE 0
#define _ZN_IS_LOCAL 1

#define _ZN_QUERYABLE_COMPLETE_DEFAULT 1
#define _ZN_QUERYABLE_DISTANCE_DEFAULT 0

typedef struct
{
    z_zint_t id;
    zn_reskey_t key;
} _zn_resource_t;

typedef struct
{
    z_zint_t id;
    zn_reskey_t key;
    zn_subinfo_t info;
    zn_data_handler_t callback;
    void *arg;
} _zn_subscriber_t;

typedef struct
{
    z_zint_t id;
    zn_reskey_t key;
} _zn_publisher_t;

typedef struct
{
    zn_reply_t reply;
    z_timestamp_t tstamp;
} _zn_pending_reply_t;

typedef struct
{
    z_zint_t id;
    zn_reskey_t key;
    const char *predicate;
    zn_query_target_t target;
    zn_query_consolidation_t consolidation;
    z_list_t *pending_replies;
    zn_query_handler_t callback;
    void *arg;
} _zn_pending_query_t;

typedef struct
{
    z_mutex_t mutex;
    z_condvar_t cond_var;
    z_vec_t replies;
} _zn_pending_query_collect_t;

typedef struct
{
    z_zint_t id;
    zn_reskey_t key;
    unsigned int kind;
    zn_queryable_handler_t callback;
    void *arg;
} _zn_queryable_t;

#endif /* _ZENOH_PICO_SESSION_PRIVATE_TYPES_H */

/* Copyright (C) 2014 Mamadou DIOP.
*
* This file is part of Open Source Doubango Framework.
*
* DOUBANGO is free software: you can redistribute it and/or modify
* it under the terms of the GNU General Public License as published by
* the Free Software Foundation, either version 3 of the License, or
* (at your option) any later version.
*
* DOUBANGO is distributed in the hope that it will be useful,
* but WITHOUT ANY WARRANTY; without even the implied warranty of
* MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
* GNU Lesser General Public License for more details.
*
* You should have received a copy of the GNU General Public License
* along with DOUBANGO.
*
*/
#include "tinybfcp/tbfcp_session.h"
#include "tinybfcp/tbfcp_pkt.h"
#include "tinybfcp/tbfcp_types.h"

#include "tsk_time.h"
#include "tsk_timer.h"
#include "tsk_object.h"
#include "tsk_memory.h"
#include "tsk_safeobj.h"
#include "tsk_debug.h"

typedef struct tbfcp_session_s {
    TSK_DECLARE_OBJECT;

    tsk_bool_t b_started;
    tsk_bool_t b_stopping;
    tsk_bool_t b_prepared;

    enum tnet_socket_type_e e_socket_type;
    char* p_local_ip;
    tnet_port_t u_local_port;

    char* p_local_public_ip;
    tnet_port_t u_local_public_port;

    char* p_remote_ip;
    tnet_port_t u_remote_port;
    struct sockaddr_storage remote_addr;

    struct tnet_ice_ctx_s* p_ice_ctx;
    struct tnet_transport_s* p_transport;

    uint8_t* p_buff_send_ptr;
    tsk_size_t u_buff_send_size;

    struct {
        tsk_timer_manager_handle_t* ph_global;
        tsk_timer_id_t id_T1; // For UDP only
        uint64_t u_timeout_T1;
        tsk_timer_id_t id_T2; // For UDP only
        uint64_t u_timeout_T2;
        tsk_timer_id_t id_TcpReconnect; // For TCP/TLS only
        uint64_t u_timeout_TcpReconnect;
    } timer;

    TSK_DECLARE_SAFEOBJ;
} tbfcp_session_t;

typedef enum _bfcp_timer_type_e {
    _bfcp_timer_type_T1, // draft-ietf-bfcpbis-rfc4582bis-11 - 4.16.  Timer Values (8.3.3)
    _bfcp_timer_type_T2, // draft-ietf-bfcpbis-rfc4582bis-11 - 4.16.  Timer Values (8.3.3)
    _bfcp_timer_type_TcpReconnect, // Try to reconnect the TCP/TLS socket every X seconds if unexpectedly disconnected
}
_bfcp_timer_type_t;

static int _tbfcp_session_socket_type_make_valid(enum tnet_socket_type_e e_in_st, enum tnet_socket_type_e *pe_out_st);
static int _tbfcp_session_timer_callback(const void* pc_arg, tsk_timer_id_t timer_id);
static int _tbfcp_session_timer_schedule(struct tbfcp_session_s* p_self, _bfcp_timer_type_t e_timer, uint64_t u_timeout);
static int _tbfcp_session_transport_layer_cb(const tnet_transport_event_t* e);

int tbfcp_session_create(tnet_socket_type_t e_socket_type, const char* pc_local_ip, tbfcp_session_t** pp_self)
{
    extern const tsk_object_def_t *tbfcp_session_def_t;
    int ret;
    tnet_socket_type_t e_st;
    tbfcp_session_t* p_self;

    if (!pp_self) {
        TSK_DEBUG_ERROR("Invalid parameter");
        return -1;
    }
    if ((ret = _tbfcp_session_socket_type_make_valid(e_socket_type, &e_st))) {
        return ret;
    }
    if (!(p_self = tsk_object_new(tbfcp_session_def_t))) {
        TSK_DEBUG_ERROR("Failed to create 'tbfcp_session_def_t' object");
        return -2;
    }
    p_self->e_socket_type = e_st;
    tsk_strupdate(&p_self->p_local_ip, pc_local_ip);
    *pp_self = p_self;
    return 0;
}

int tbfcp_session_create_2(struct tnet_ice_ctx_s* p_ice_ctx, tbfcp_session_t** pp_self)
{
    tnet_socket_type_t e_st;
    const char* pc_local_ip;
    int ret;
    if (!pp_self || !p_ice_ctx) {
        TSK_DEBUG_ERROR("Invalid parameter");
        return -1;
    }

    // TODO: ICE expect RTP and RTCP comp-ids only
    TSK_DEBUG_ERROR("Not supported yet");
    return -2;

    // TODO: For now ICE works with UDP (and DTLS) only
    e_st = tnet_ice_ctx_use_ipv6(p_ice_ctx) ? tnet_socket_type_udp_ipv4 : tnet_socket_type_udp_ipv6;
    pc_local_ip = tnet_ice_ctx_use_ipv6(p_ice_ctx) ? "::1" : "127.0.0.1";
    if ((ret = _tbfcp_session_socket_type_make_valid(e_st, &e_st))) {
        return ret;
    }
    if ((ret = tbfcp_session_create(e_st, pc_local_ip, pp_self))) {
        return ret;
    }
    (*pp_self)->p_ice_ctx = tsk_object_ref(p_ice_ctx);
    return 0;
}

int tbfcp_session_set_ice_ctx(tbfcp_session_t* p_self, struct tnet_ice_ctx_s* p_ice_ctx)
{
    if (!p_self) {
        TSK_DEBUG_ERROR("Invalid parameter");
        return -1;
    }
    TSK_OBJECT_SAFE_FREE(p_self->p_ice_ctx);
    if (p_ice_ctx) {
        p_self->p_ice_ctx = tsk_object_ref(p_ice_ctx);
    }
    return 0;
}

int tbfcp_session_prepare(tbfcp_session_t* p_self)
{
    int ret = 0;
    const char *bfcp_local_ip;
    tnet_port_t bfcp_local_port;

    if (!p_self) {
        TSK_DEBUG_ERROR("Invalid parameter");
        return -1;
    }

    // lock()
    tsk_safeobj_lock(p_self);

    if (p_self->b_prepared) {
        TSK_DEBUG_INFO("BFCP session already prepared");
        goto bail;
    }
    if (p_self->p_transport) {
        TSK_DEBUG_ERROR("BFCP session already has a transport. Unexpected.");
        ret = -2;
        goto bail;
    }

    bfcp_local_ip = TNET_SOCKET_HOST_ANY;
    bfcp_local_port = TNET_SOCKET_PORT_ANY;

    if (p_self->p_ice_ctx) {
        // Get Sockets when the transport is started
        bfcp_local_ip = tnet_ice_ctx_use_ipv6(p_self->p_ice_ctx) ? "::1" : "127.0.0.1";
    }
    else {
        // create transport
        if (!(p_self->p_transport = tnet_transport_create(p_self->p_local_ip, p_self->u_local_port, p_self->e_socket_type, kBfcpTransportFriendlyName))) {
            TSK_DEBUG_ERROR("Failed to create %s Transport", kBfcpTransportFriendlyName);
            return -3;
        }
        // set transport callback
        if ((ret = tnet_transport_set_callback(p_self->p_transport, _tbfcp_session_transport_layer_cb, p_self))) {
            goto bail;
        }
        bfcp_local_ip = p_self->p_transport->master->ip;
        bfcp_local_port = p_self->p_transport->master->port;
    }

    tsk_strupdate(&p_self->p_local_public_ip, bfcp_local_ip);
    p_self->u_local_public_port = bfcp_local_port;

    p_self->b_prepared = tsk_true;

bail:
    // unlock()
    tsk_safeobj_unlock(p_self);
    return ret;
}

int tbfcp_session_start(tbfcp_session_t* p_self)
{
    int ret;
    if (!p_self) {
        TSK_DEBUG_ERROR("Invalid parameter");
        return -1;
    }

    // lock()
    tsk_safeobj_lock(p_self);

    if (p_self->b_started) {
        TSK_DEBUG_INFO("BFCP session already started");
        ret = 0;
        goto bail;
    }

    if (!p_self->b_prepared) {
        TSK_DEBUG_ERROR("BFCP session not prepared yet");
        ret = -2;
        goto bail;
    }

    // start global timer manager - nothing will be done if already started by another component
    if ((ret = tsk_timer_manager_start(p_self->timer.ph_global))) {
        TSK_DEBUG_ERROR("Failed to start timer");
        goto bail;
    }

    // check remote IP address validity
    if ((tsk_striequals(p_self->p_remote_ip, "0.0.0.0") || tsk_striequals(p_self->p_remote_ip, "::"))) { // most likely loopback testing
        tnet_ip_t source = {0};
        tsk_bool_t updated = tsk_false;
        if (p_self->p_transport && p_self->p_transport->master) {
            updated = (tnet_getbestsource(p_self->p_transport->master->ip, p_self->p_transport->master->port, p_self->p_transport->master->type, &source) == 0);
        }
        // Not allowed to send data to "0.0.0.0"
        TSK_DEBUG_INFO("BFCP remote IP contains not allowed value ...changing to '%s'", updated ? source : "oops");
        if (updated) {
            tsk_strupdate(&p_self->p_remote_ip, source);
        }
    }
    // init remote remote addr
    if ((ret = tnet_sockaddr_init(p_self->p_remote_ip, p_self->u_remote_port, p_self->p_transport->master->type, &p_self->remote_addr))) {
        tnet_transport_shutdown(p_self->p_transport);
        TSK_OBJECT_SAFE_FREE(p_self->p_transport);
        TSK_DEBUG_ERROR("Invalid BFCP host:port [%s:%u]", p_self->p_remote_ip, p_self->u_remote_port);
        goto bail;
    }

    // start the transport
    if ((ret = tnet_transport_start(p_self->p_transport))) {
        TSK_DEBUG_ERROR("Failed to start the %s transport", kBfcpTransportFriendlyName);
        goto bail;
    }

    p_self->b_started = tsk_true;

bail:
    // unlock()
    tsk_safeobj_unlock(p_self);
    return ret;
}

int tbfcp_session_stop(tbfcp_session_t* p_self)
{
    int ret = 0;
    if (!p_self) {
        TSK_DEBUG_ERROR("Invalid parameter");
        return -1;
    }

    // lock()
    tsk_safeobj_lock(p_self);

    // stop the session if not already done
    if (!p_self->b_started) {
        TSK_DEBUG_INFO("BFCP session already stopped");
        goto bail;
    }
    p_self->b_stopping = tsk_true;
    // this is a global timer shared by many components -> stopping it won't remove
    // all scheduled items as it could continue running if still used
    if (TSK_TIMER_ID_IS_VALID(p_self->timer.id_T1)) {
        tsk_timer_manager_cancel(p_self->timer.ph_global, p_self->timer.id_T1);
        p_self->timer.id_T1 = TSK_INVALID_TIMER_ID;
    }
    if (TSK_TIMER_ID_IS_VALID(p_self->timer.id_T2)) {
        tsk_timer_manager_cancel(p_self->timer.ph_global, p_self->timer.id_T2);
        p_self->timer.id_T2 = TSK_INVALID_TIMER_ID;
    }
    if (TSK_TIMER_ID_IS_VALID(p_self->timer.id_TcpReconnect)) {
        tsk_timer_manager_cancel(p_self->timer.ph_global, p_self->timer.id_TcpReconnect);
        p_self->timer.id_TcpReconnect = TSK_INVALID_TIMER_ID;
    }

    // free transport to force next call to start() to create new one with new sockets
    if (p_self->p_transport) {
        tnet_transport_shutdown(p_self->p_transport);
        TSK_OBJECT_SAFE_FREE(p_self->p_transport);
    }

    p_self->b_started = tsk_false;
    p_self->b_stopping = tsk_false;

bail:
    // unlock()
    tsk_safeobj_unlock(p_self);
    return ret;
}

int tbfcp_session_set_remote(tbfcp_session_t* p_self, const char* pc_ip, tnet_port_t u_port)
{
    if (!p_self || !pc_ip /*|| u_port < 1024*/) {
        TSK_DEBUG_ERROR("Invalid parameter");
        return -1;
    }
    // if ICE is enabled then, these values will be updated when the session start()s and call ice_init()
    tsk_strupdate(&p_self->p_remote_ip, pc_ip);
    p_self->u_remote_port = u_port;
    return 0;
}

int tbfcp_session_get_profile(const tbfcp_session_t* pc_self, const char** ppc_profile)
{
    if (!pc_self) {
        TSK_DEBUG_ERROR("Invalid parameter");
        return -1;
    }
    return tbfcp_session_get_profile_2(pc_self->e_socket_type, ppc_profile);
}


int tbfcp_session_get_profile_2(enum tnet_socket_type_e e_socket_type, const char** ppc_profile)
{
    if (!ppc_profile) {
        TSK_DEBUG_ERROR("Invalid parameter");
        return -1;
    }

    if (TNET_SOCKET_TYPE_IS_DTLS(e_socket_type)) {
        *ppc_profile = kBfcpProfileDTLS;
        return 0;
    }
    if (TNET_SOCKET_TYPE_IS_UDP(e_socket_type)) {
        *ppc_profile = kBfcpProfileUDP;
        return 0;
    }
    if (TNET_SOCKET_TYPE_IS_TLS(e_socket_type)) {
        *ppc_profile = kBfcpProfileTLS;
        return 0;
    }
    if (TNET_SOCKET_TYPE_IS_TCP(e_socket_type)) {
        *ppc_profile = kBfcpProfileTCP;
        return 0;
    }
    TSK_DEBUG_ERROR("%d not supported as valid socket type for BFCP sessions", e_socket_type);
    return -2;
}

static int _tbfcp_session_send_buff(tbfcp_session_t* p_self, const void* pc_buff_ptr, tsk_size_t u_buff_size)
{
    int ret = 0;
    tsk_size_t u_sent_bytes = 0;
    if (!p_self || !pc_buff_ptr || !u_buff_size) {
        TSK_DEBUG_ERROR("Invalid parameter");
        return -1;
    }

    // lock()
    tsk_safeobj_lock(p_self);

    if (!p_self->b_started) {
        TSK_DEBUG_ERROR("BFCP session not started");
        ret = -2;
        goto bail;
    }

    if (TNET_SOCKET_TYPE_IS_DGRAM(p_self->e_socket_type)) {
        u_sent_bytes = tnet_transport_sendto(p_self->p_transport, p_self->p_transport->master->fd, (const struct sockaddr *)&p_self->remote_addr, pc_buff_ptr, u_buff_size);
    }
    else {
    }

    if (u_sent_bytes != u_buff_size) {
        TSK_DEBUG_ERROR("Failed to send %u bytes. Only %u sent", u_buff_size, u_sent_bytes);
        ret = -2;
        goto bail;
    }

bail:
    // unlock()
    tsk_safeobj_unlock(p_self);
    return ret;
}

int tbfcp_session_send_pkt(tbfcp_session_t* p_self, const tbfcp_pkt_t* pc_pkt)
{
    int ret;
    tsk_size_t u_min_size;
    if (!p_self || !pc_pkt) {
        TSK_DEBUG_ERROR("Invalid parameter");
        return -1;
    }

    // lock()
    tsk_safeobj_lock(p_self);

    if (!p_self->b_started) {
        TSK_DEBUG_ERROR("BFCP session not started");
        ret = -2;
        goto bail;
    }
    if ((ret = tbfcp_pkt_get_size_in_octetunits_with_padding(pc_pkt, &u_min_size))) {
        goto bail;
    }
    u_min_size += kBfcpBuffMinPad;
    if (p_self->u_buff_send_size < u_min_size) {
        if (!(p_self->p_buff_send_ptr = tsk_realloc(p_self->p_buff_send_ptr, u_min_size))) {
            TSK_DEBUG_ERROR("Failed to allocate buffer with size = %u", u_min_size);
            ret = -3;
            p_self->u_buff_send_size = 0;
            goto bail;
        }
        p_self->u_buff_send_size = u_min_size;
    }

    if ((ret = tbfcp_pkt_write_with_padding(pc_pkt, p_self->p_buff_send_ptr, p_self->u_buff_send_size, &u_min_size))) {
        goto bail;
    }
    if ((ret = _tbfcp_session_send_buff(p_self, p_self->p_buff_send_ptr, u_min_size))) {
        goto bail;
    }

bail:
    // unlock()
    tsk_safeobj_unlock(p_self);
    return ret;
}

// clear junks (e.g. IPSec)
static int _tbfcp_session_socket_type_make_valid(enum tnet_socket_type_e e_in_st, enum tnet_socket_type_e *pe_out_st)
{
    if (!pe_out_st) {
        TSK_DEBUG_ERROR("Invalid parameter");
        return -1;
    }
    if (TNET_SOCKET_TYPE_IS_DTLS(e_in_st) || TNET_SOCKET_TYPE_IS_UDP(e_in_st) || TNET_SOCKET_TYPE_IS_TLS(e_in_st) || TNET_SOCKET_TYPE_IS_TCP(e_in_st)) {
        *pe_out_st = e_in_st;
    }
    else {
        *pe_out_st = kBfcpTransportDefault;
    }
    return 0;
}

static int _tbfcp_session_timer_schedule(tbfcp_session_t* p_self, _bfcp_timer_type_t e_timer, uint64_t u_timeout)
{
    int ret = 0;
    if (!p_self) {
        TSK_DEBUG_ERROR("Invalid parameter");
        return -1;
    }

    tsk_safeobj_lock(p_self); // must
    switch (e_timer) {
    case _bfcp_timer_type_T1:
        if (!TSK_TIMER_ID_IS_VALID(p_self->timer.id_T1)) {
            p_self->timer.id_T1 = tsk_timer_mgr_global_schedule(u_timeout, _tbfcp_session_timer_callback, p_self);
        }
        else {
            TSK_DEBUG_ERROR("T1 timer is still valid");
            ret = -3;
        }
        break;
    case _bfcp_timer_type_T2:
        if (!TSK_TIMER_ID_IS_VALID(p_self->timer.id_T2)) {
            p_self->timer.id_T2 = tsk_timer_mgr_global_schedule(u_timeout, _tbfcp_session_timer_callback, p_self);
        }
        else {
            TSK_DEBUG_ERROR("T2 timer is still valid");
            ret = -3;
        }
        break;
    case _bfcp_timer_type_TcpReconnect:
        if (!TSK_TIMER_ID_IS_VALID(p_self->timer.id_TcpReconnect)) {
            p_self->timer.id_TcpReconnect = tsk_timer_mgr_global_schedule(u_timeout, _tbfcp_session_timer_callback, p_self);
        }
        else {
            TSK_DEBUG_ERROR("TcpReconnect timer is still valid");
            ret = -3;
        }
        break;
    default:
        TSK_DEBUG_ERROR("%d not valid BFCP timer", e_timer);
        ret = -2;
        break;
    }
    tsk_safeobj_unlock(p_self);
    return ret;
}

static int _tbfcp_session_timer_callback(const void* pc_arg, tsk_timer_id_t timer_id)
{
    tbfcp_session_t* p_session = (tbfcp_session_t*)pc_arg;
    tsk_safeobj_lock(p_session); // must
    if (p_session->timer.id_T1 == timer_id) {
        p_session->timer.id_T1 = TSK_INVALID_TIMER_ID;
        // OnExpire(session, EVENT_BYE);
    }
    else if (p_session->timer.id_T2 == timer_id) {
        p_session->timer.id_T2 = TSK_INVALID_TIMER_ID;
        // OnExpire(session, EVENT_REPORT);
    }
    tsk_safeobj_unlock(p_session);
    return 0;
}

static int _tbfcp_session_transport_layer_cb(const tnet_transport_event_t* e)
{
    tbfcp_session_t* p_session = (tbfcp_session_t*)e->callback_data;

    return 0;
}

static tsk_object_t* tbfcp_session_ctor(tsk_object_t * self, va_list * app)
{
    tbfcp_session_t *p_session = (tbfcp_session_t *)self;
    if (p_session) {
        p_session->timer.id_T1 = TSK_INVALID_TIMER_ID;
        p_session->timer.id_T2 = TSK_INVALID_TIMER_ID;
        // get a handle for the global timer manager
        if (!(p_session->timer.ph_global = tsk_timer_mgr_global_ref())) {
            TSK_DEBUG_ERROR("Failed to get a reference to the global timer");
            return tsk_null;
        }
        p_session->u_local_port = TNET_SOCKET_PORT_ANY;
        tsk_safeobj_init(p_session);
    }
    return self;
}
static tsk_object_t* tbfcp_session_dtor(tsk_object_t * self)
{
    tbfcp_session_t *p_session = (tbfcp_session_t *)self;
    if (p_session) {
        TSK_DEBUG_INFO("*** BFCP session destroyed ***");
        // stop the session if not already done
        tbfcp_session_stop(p_session);
        // release the handle for the global timer manager
        if (p_session->timer.ph_global) {
            tsk_timer_mgr_global_unref(&p_session->timer.ph_global);
        }
        TSK_FREE(p_session->p_local_ip);
        TSK_FREE(p_session->p_local_public_ip);
        TSK_FREE(p_session->p_remote_ip);
        TSK_FREE(p_session->p_buff_send_ptr);
        TSK_OBJECT_SAFE_FREE(p_session->p_ice_ctx);
        TSK_OBJECT_SAFE_FREE(p_session->p_transport);
        tsk_safeobj_deinit(p_session);
    }

    return self;
}
static int tbfcp_session_cmp(const tsk_object_t *_pss1, const tsk_object_t *_pss2)
{
    const tbfcp_session_t *pc_ss1 = (const tbfcp_session_t *)_pss1;
    const tbfcp_session_t *pc_ss2 = (const tbfcp_session_t *)_pss2;
    return (int)(pc_ss1 - pc_ss2);
}
static const tsk_object_def_t tbfcp_session_def_s = {
    sizeof(tbfcp_session_t),
    tbfcp_session_ctor,
    tbfcp_session_dtor,
    tbfcp_session_cmp,
};
const tsk_object_def_t *tbfcp_session_def_t = &tbfcp_session_def_s;
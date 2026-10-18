/*
 * Copyright (c) 2026 The dya studio contributors
 *
 * SPDX-License-Identifier: MIT
 *
 * Generic ZMK Studio RPC extension subsystem.
 *
 * Registers a single `zmk_rpc_subsystem` for the generated
 * `zmk_studio_Request_extension_tag` arm (tag 6) and dispatches on the
 * Extension (namespace, type) pair. ZMK core is left untouched: this only uses
 * the public API in <zmk/studio/rpc.h> and the generated studio.pb.h.
 *
 * Security: our subsystem func is called directly by rpc.c's handle_request(),
 * bypassing zmk_rpc_subsystem_delegate_to_subs(), so the lock check that the
 * built-in subsystems get for free is implemented here, per namespace.
 */

#include <zephyr/kernel.h>
#include <zephyr/init.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/util.h>

#include <string.h>

#include <pb_decode.h>
#include <pb_encode.h>

#include <zmk/studio/rpc.h>
#include <zmk/studio/core.h>

#include <proto/zmk/studio.pb.h>

#include <zmk/ext/ext.pb.h>
#include <dya/status/status.pb.h>
#include <dya/secured/secured.pb.h>

#include <zmk_feature_dya_studio/dya_studio.h>

LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

#if !IS_ENABLED(CONFIG_ZMK_SPLIT) || IS_ENABLED(CONFIG_ZMK_SPLIT_ROLE_CENTRAL)

enum dya_dispatch_result {
    DYA_OK,
    DYA_ERR_NOT_FOUND,
    DYA_ERR_UNLOCK_REQUIRED,
    DYA_ERR_DECODE_FAILED,
    DYA_ERR_ENCODE_FAILED,
};

struct dya_namespace_meta {
    const char *name;
    bool secured;
};

static const struct dya_namespace_meta dya_namespaces[] = {
    {DYA_EXT_NS_EXT, false},
    {DYA_EXT_NS_STATUS, false},
    {DYA_EXT_NS_SECURED, true},
};

#define DYA_NAMESPACE_COUNT ARRAY_SIZE(dya_namespaces)

static const struct dya_namespace_meta *find_namespace(const char *name) {
    for (size_t i = 0; i < DYA_NAMESPACE_COUNT; i++) {
        if (strcmp(dya_namespaces[i].name, name) == 0) {
            return &dya_namespaces[i];
        }
    }
    return NULL;
}

static void set_response_target(zmk_studio_Extension *out, const char *ns, uint32_t type) {
    strncpy(out->namespace, ns, sizeof(out->namespace) - 1);
    out->namespace[sizeof(out->namespace) - 1] = '\0';
    out->type = type;
}

static bool decode_request(const zmk_studio_Extension *ext, const pb_msgdesc_t *fields,
                           void *dest) {
    pb_istream_t stream = pb_istream_from_buffer(ext->payload.bytes, ext->payload.size);
    if (!pb_decode(&stream, fields, dest)) {
        LOG_WRN("Failed to decode extension request: %s", PB_GET_ERROR(&stream));
        return false;
    }
    return true;
}

static enum dya_dispatch_result encode_payload(const zmk_studio_Extension *req,
                                               zmk_studio_Extension *out, const char *ns,
                                               const pb_msgdesc_t *fields, const void *msg) {
    pb_ostream_t stream = pb_ostream_from_buffer(out->payload.bytes, sizeof(out->payload.bytes));
    if (!pb_encode(&stream, fields, msg)) {
        LOG_WRN("Failed to encode extension response: %s", PB_GET_ERROR(&stream));
        return DYA_ERR_ENCODE_FAILED;
    }
    set_response_target(out, ns, req->type);
    out->payload.size = (pb_size_t)stream.bytes_written;
    return DYA_OK;
}

static enum dya_dispatch_result handle_zmk_ext(const zmk_studio_Extension *req,
                                               zmk_studio_Extension *out) {
    switch (req->type) {
    case DYA_EXT_OP_GET_PROTOCOL_VERSION: {
        zmk_ext_GetProtocolVersionRequest request = zmk_ext_GetProtocolVersionRequest_init_zero;
        if (!decode_request(req, zmk_ext_GetProtocolVersionRequest_fields, &request)) {
            return DYA_ERR_DECODE_FAILED;
        }
        zmk_ext_GetProtocolVersionResponse response =
            zmk_ext_GetProtocolVersionResponse_init_zero;
        response.protocol_version = DYA_EXT_PROTOCOL_VERSION;
        return encode_payload(req, out, DYA_EXT_NS_EXT,
                              zmk_ext_GetProtocolVersionResponse_fields, &response);
    }
    case DYA_EXT_OP_LIST_NAMESPACES: {
        zmk_ext_ListNamespacesRequest request = zmk_ext_ListNamespacesRequest_init_zero;
        if (!decode_request(req, zmk_ext_ListNamespacesRequest_fields, &request)) {
            return DYA_ERR_DECODE_FAILED;
        }
        zmk_ext_ListNamespacesResponse response = zmk_ext_ListNamespacesResponse_init_zero;
        for (size_t i = 0; i < DYA_NAMESPACE_COUNT; i++) {
            strncpy(response.namespaces[response.namespaces_count], dya_namespaces[i].name,
                    sizeof(response.namespaces[0]) - 1);
            response.namespaces_count++;
        }
        return encode_payload(req, out, DYA_EXT_NS_EXT, zmk_ext_ListNamespacesResponse_fields,
                              &response);
    }
    default:
        return DYA_ERR_NOT_FOUND;
    }
}

static enum dya_dispatch_result handle_status(const zmk_studio_Extension *req,
                                              zmk_studio_Extension *out) {
    switch (req->type) {
    case DYA_STATUS_OP_GET_STATUS: {
        dya_status_GetStatusRequest request = dya_status_GetStatusRequest_init_zero;
        if (!decode_request(req, dya_status_GetStatusRequest_fields, &request)) {
            return DYA_ERR_DECODE_FAILED;
        }
        dya_status_GetStatusResponse response = dya_status_GetStatusResponse_init_zero;
        response.protocol_version = DYA_EXT_PROTOCOL_VERSION;
#ifdef CONFIG_ZMK_KEYBOARD_NAME
        strncpy(response.controller_name, CONFIG_ZMK_KEYBOARD_NAME,
                sizeof(response.controller_name) - 1);
#else
        strncpy(response.controller_name, "dya-studio", sizeof(response.controller_name) - 1);
#endif
        return encode_payload(req, out, DYA_EXT_NS_STATUS, dya_status_GetStatusResponse_fields,
                              &response);
    }
    default:
        return DYA_ERR_NOT_FOUND;
    }
}

static enum dya_dispatch_result handle_secured(const zmk_studio_Extension *req,
                                               zmk_studio_Extension *out) {
    switch (req->type) {
    case DYA_SECURED_OP_PING: {
        dya_secured_PingRequest request = dya_secured_PingRequest_init_zero;
        if (!decode_request(req, dya_secured_PingRequest_fields, &request)) {
            return DYA_ERR_DECODE_FAILED;
        }
        dya_secured_PingResponse response = dya_secured_PingResponse_init_zero;
        strncpy(response.message, request.message, sizeof(response.message) - 1);
        return encode_payload(req, out, DYA_EXT_NS_SECURED, dya_secured_PingResponse_fields,
                              &response);
    }
    default:
        return DYA_ERR_NOT_FOUND;
    }
}

static enum dya_dispatch_result dya_dispatch(const zmk_studio_Extension *req,
                                             zmk_studio_Extension *out) {
    const struct dya_namespace_meta *meta = find_namespace(req->namespace);
    if (meta == NULL) {
        LOG_DBG("Unknown extension namespace: %s", req->namespace);
        return DYA_ERR_NOT_FOUND;
    }

    if (meta->secured &&
        zmk_studio_core_get_lock_state() != ZMK_STUDIO_CORE_LOCK_STATE_UNLOCKED) {
        return DYA_ERR_UNLOCK_REQUIRED;
    }

    if (strcmp(meta->name, DYA_EXT_NS_EXT) == 0) {
        return handle_zmk_ext(req, out);
    }
    if (strcmp(meta->name, DYA_EXT_NS_STATUS) == 0) {
        return handle_status(req, out);
    }
    if (strcmp(meta->name, DYA_EXT_NS_SECURED) == 0) {
        return handle_secured(req, out);
    }
    return DYA_ERR_NOT_FOUND;
}

static zmk_studio_Response extension_subsystem_func(const struct zmk_rpc_subsystem *subsys,
                                                    const zmk_studio_Request *req) {
    ARG_UNUSED(subsys);
    const zmk_studio_Extension *ext = &req->subsystem.extension;

    zmk_studio_Extension resp_ext = zmk_studio_Extension_init_zero;

    switch (dya_dispatch(ext, &resp_ext)) {
    case DYA_OK: {
        zmk_studio_Response resp = zmk_studio_Response_init_zero;
        resp.which_type = zmk_studio_Response_request_response_tag;
        resp.type.request_response.which_subsystem = zmk_studio_RequestResponse_extension_tag;
        resp.type.request_response.subsystem.extension = resp_ext;
        return resp;
    }
    case DYA_ERR_UNLOCK_REQUIRED:
        return ZMK_RPC_SIMPLE_ERR(UNLOCK_REQUIRED);
    case DYA_ERR_DECODE_FAILED:
        return ZMK_RPC_SIMPLE_ERR(MSG_DECODE_FAILED);
    case DYA_ERR_ENCODE_FAILED:
        return ZMK_RPC_SIMPLE_ERR(MSG_ENCODE_FAILED);
    case DYA_ERR_NOT_FOUND:
    default:
        return ZMK_RPC_SIMPLE_ERR(RPC_NOT_FOUND);
    }
}

STRUCT_SECTION_ITERABLE(zmk_rpc_subsystem, dya_extension_subsystem) = {
    .func = extension_subsystem_func,
    .subsystem_choice = zmk_studio_Request_extension_tag,
};

static int dya_studio_init(void) {
    LOG_INF("dya studio extender: protocol v%d, %d namespaces", DYA_EXT_PROTOCOL_VERSION,
            (int)DYA_NAMESPACE_COUNT);
    for (size_t i = 0; i < DYA_NAMESPACE_COUNT; i++) {
        LOG_DBG("  namespace: %s%s", dya_namespaces[i].name,
                dya_namespaces[i].secured ? " (secured)" : "");
    }
    return 0;
}

SYS_INIT(dya_studio_init, APPLICATION, 0);

#endif /* !CONFIG_ZMK_SPLIT || CONFIG_ZMK_SPLIT_ROLE_CENTRAL */

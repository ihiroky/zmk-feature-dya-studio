/*
 * SPDX-License-Identifier: MIT
 *
 * Public constants for the dya studio protocol extender.
 *
 * The extension RPC rides on the generic Studio `Extension` arm:
 *   zmk.studio.Extension { string namespace; uint32 type; bytes payload; }
 *
 * This header only exposes the reserved namespaces and their operation ids so
 * that CLI / UI / tests can share them.
 */

#pragma once

/* Bump when the extension wire contract changes in a breaking way. */
#define DYA_EXT_PROTOCOL_VERSION 1

/* Namespace reserved by the extender itself for feature discovery. */
#define DYA_EXT_NS_EXT "zmk.ext"
/* Type ids within DYA_EXT_NS_EXT. */
#define DYA_EXT_OP_GET_PROTOCOL_VERSION 0
#define DYA_EXT_OP_LIST_NAMESPACES 1

/* Example read-only feature namespace. */
#define DYA_EXT_NS_STATUS "dya.status"
#define DYA_STATUS_OP_GET_STATUS 0

/* Example secured namespace: rejected while the device is locked. */
#define DYA_EXT_NS_SECURED "dya.secured"
#define DYA_SECURED_OP_PING 0

/*
 * Copyright (c) 2013-2015 Intel Corporation, Inc.  All rights reserved.
 * Copyright (c) 2018-2019 Cray Inc. All rights reserved.
 * Copyright (c) 2018-2019 System Fabric Works, Inc. All rights reserved.
 * Copyright (c) 2025 VDURA, Inc. All rights reserved.
 *
 * This software is available to you under a choice of one of two
 * licenses.  You may choose to be licensed under the terms of the GNU
 * General Public License (GPL) Version 2, available from the file
 * COPYING in the main directory of this source tree, or the
 * BSD license below:
 *
 *     Redistribution and use in source and binary forms, with or
 *     without modification, are permitted provided that the following
 *     conditions are met:
 *
 *      - Redistributions of source code must retain the above
 *        copyright notice, this list of conditions and the following
 *        disclaimer.
 *
 *      - Redistributions in binary form must reproduce the above
 *        copyright notice, this list of conditions and the following
 *        disclaimer in the documentation and/or other materials
 *        provided with the distribution.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
 * NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS
 * BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN
 * ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN
 * CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 */

#include "config.h"

#include <ofi_util.h>
#include "verbs_ofi.h"

/* XRC SIDR connection map RBTree key */
struct vrb_sidr_conn_key {
	struct sockaddr		*addr;
	uint16_t		pep_port;
	bool			recip;
};

static ssize_t
vrb_eq_readerr(struct fid_eq *eq, struct fi_eq_err_entry *entry,
		  uint64_t flags)
{
	struct vrb_eq *_eq =
		container_of(eq, struct vrb_eq, eq_fid.fid);
	ssize_t rd = -FI_EAGAIN;
	ofi_mutex_lock(&_eq->lock);
	if (!_eq->err.err)
		goto unlock;

	ofi_eq_handle_err_entry(_eq->fab->util_fabric.fabric_fid.api_version,
				flags, &_eq->err, entry);
	rd = sizeof(*entry);
unlock:
	ofi_mutex_unlock(&_eq->lock);
	return rd;
}

void vrb_eq_set_xrc_conn_tag(struct vrb_xrc_ep *ep)
{
	struct vrb_eq *eq = ep->base_ep.eq;

	assert(ofi_mutex_held(&eq->event_lock));
	assert(ep->conn_setup);
	assert(ep->conn_setup->conn_tag == VERBS_CONN_TAG_INVALID);
	ep->conn_setup->conn_tag =
		(uint32_t)ofi_idx2key(&eq->xrc.conn_key_idx,
				ofi_idx_insert(eq->xrc.conn_key_map, ep));
}

void vrb_eq_clear_xrc_conn_tag(struct vrb_xrc_ep *ep)
{
	struct vrb_eq *eq = ep->base_ep.eq;
	int index;

	assert(ofi_mutex_held(&eq->event_lock));
	assert(ep->conn_setup);
	if (ep->conn_setup->conn_tag == VERBS_CONN_TAG_INVALID)
		return;

	index = ofi_key2idx(&eq->xrc.conn_key_idx,
			    (uint64_t)ep->conn_setup->conn_tag);
	if (!ofi_idx_is_valid(eq->xrc.conn_key_map, index))
	    VRB_WARN(FI_LOG_EP_CTRL,
		       "Invalid XRC connection connection tag\n");
	else
		ofi_idx_remove(eq->xrc.conn_key_map, index);
	ep->conn_setup->conn_tag = VERBS_CONN_TAG_INVALID;
}

static struct vrb_xrc_ep *
vrb_eq_xrc_conn_tag2ep(struct vrb_eq *eq, uint32_t conn_tag)
{
	struct vrb_xrc_ep *ep;
	int index;

	assert(ofi_mutex_held(&eq->event_lock));
	index = ofi_key2idx(&eq->xrc.conn_key_idx, (uint64_t)conn_tag);
	ep = ofi_idx_lookup(eq->xrc.conn_key_map, index);
	if (!ep || ep->magic != VERBS_XRC_EP_MAGIC) {
		VRB_WARN(FI_LOG_EP_CTRL, "XRC EP is not valid\n");
		return NULL;
	}
	if (!ep->conn_setup) {
		VRB_WARN(FI_LOG_EP_CTRL,
			   "Bad state, no connection data\n");
		return NULL;
	}
	if (ep->conn_setup->conn_tag != conn_tag) {
		VRB_WARN(FI_LOG_EP_CTRL, "Connection tag mismatch\n");
		return NULL;
	}

	ofi_idx_remove(eq->xrc.conn_key_map, index);
	ep->conn_setup->conn_tag = VERBS_CONN_TAG_INVALID;

	return ep;
}

static int vrb_eq_set_xrc_info(struct rdma_cm_event *event,
				  struct vrb_xrc_conn_info *info)
{
	struct vrb_xrc_cm_data *remote = (struct vrb_xrc_cm_data *)
						event->param.conn.private_data;
	int ret;

	ret = vrb_verify_xrc_cm_data(remote,
					event->param.conn.private_data_len);
	if (ret)
		return ret;

	info->is_reciprocal = remote->reciprocal;
	info->conn_tag = ntohl(remote->conn_tag);
	info->port = ntohs(remote->port);
	info->tgt_qpn = ntohl(remote->tgt_qpn);
	info->peer_srqn = ntohl(remote->srqn);
	info->conn_param = event->param.conn;
	info->conn_param.private_data = NULL;
	info->conn_param.private_data_len = 0;

	return FI_SUCCESS;
}

static int
vrb_pep_dev_domain_match(struct fi_info *hints, const char *devname)
{
	int ret;

	if ((VRB_EP_PROTO(hints)) == FI_PROTO_RDMA_CM_IB_XRC)
		ret = vrb_cmp_xrc_domain_name(hints->domain_attr->name,
						 devname);
	else
		ret = strcmp(hints->domain_attr->name, devname);

	return ret;
}

static int
vrb_eq_cm_getinfo(struct rdma_cm_event *event, struct fi_info *pep_info,
		     struct fi_info **info)
{
	struct fi_info *hints;
	struct vrb_connreq *connreq;
	const char *devname = ibv_get_device_name(event->id->verbs->device);
	int ret = -FI_ENOMEM;

	if (!(hints = fi_dupinfo(pep_info))) {
		VRB_WARN(FI_LOG_EP_CTRL, "dupinfo failure\n");
		return -FI_ENOMEM;
	}

	/* Free src_addr info from pep to avoid addr reuse errors */
	free(hints->src_addr);
	hints->src_addr = NULL;
	hints->src_addrlen = 0;

	if (!strcmp(hints->domain_attr->name, VERBS_ANY_DOMAIN)) {
		free(hints->domain_attr->name);
		if (!(hints->domain_attr->name = strdup(devname)))
			goto err1;
	} else {
		if (vrb_pep_dev_domain_match(hints, devname)) {
			VRB_WARN(FI_LOG_EQ, "passive endpoint domain: %s does"
				   " not match device: %s where we got a "
				   "connection request\n",
				   hints->domain_attr->name, devname);
			ret = -FI_ENODATA;
			goto err1;
		}
	}

	if (!strcmp(hints->domain_attr->name, VERBS_ANY_FABRIC)) {
		free(hints->fabric_attr->name);
		hints->fabric_attr->name = NULL;
	}

	ofi_mutex_lock(&vrb_info_mutex);
	ret = vrb_get_matching_info(hints->fabric_attr->api_version, hints,
				    info, vrb_util_prov.info, 0);
	ofi_mutex_unlock(&vrb_info_mutex);
	if (ret)
		goto err1;

	ofi_alter_info(*info, hints, hints->fabric_attr->api_version);
	vrb_alter_info(hints, *info);
	(*info)->fabric_attr->api_version = pep_info->fabric_attr->api_version;
	(*info)->fabric_attr->prov_name = strdup(pep_info->fabric_attr->prov_name);
	if (!(*info)->fabric_attr->prov_name)
		goto err2;

	free((*info)->src_addr);
	(*info)->src_addrlen = ofi_sizeofaddr(rdma_get_local_addr(event->id));
	(*info)->src_addr = malloc((*info)->src_addrlen);
	if (!((*info)->src_addr))
		goto err2;
	memcpy((*info)->src_addr, rdma_get_local_addr(event->id), (*info)->src_addrlen);

	assert(!(*info)->dest_addr);
	(*info)->dest_addrlen = ofi_sizeofaddr(rdma_get_peer_addr(event->id));
	(*info)->dest_addr = malloc((*info)->dest_addrlen);
	if (!((*info)->dest_addr))
		goto err2;
	memcpy((*info)->dest_addr, rdma_get_peer_addr(event->id), (*info)->dest_addrlen);

	ofi_straddr_dbg(&vrb_prov, FI_LOG_EQ, "src", (*info)->src_addr);
	ofi_straddr_dbg(&vrb_prov, FI_LOG_EQ, "dst", (*info)->dest_addr);

	connreq = calloc(1, sizeof *connreq);
	if (!connreq) {
		VRB_WARN(FI_LOG_EP_CTRL,
			   "Unable to allocate connreq memory\n");
		goto err2;
	}

	connreq->handle.fclass = FI_CLASS_CONNREQ;
	connreq->id = event->id;

	if (vrb_is_xrc_info(*info)) {
		connreq->is_xrc = 1;
		ret = vrb_eq_set_xrc_info(event, &connreq->xrc);
		if (ret)
			goto err3;
	}

	(*info)->handle = &connreq->handle;
	fi_freeinfo(hints);
	return 0;

err3:
	free(connreq);
err2:
	fi_freeinfo(*info);
err1:
	fi_freeinfo(hints);
	return ret;
}

static inline int vrb_eq_copy_event_data(struct fi_eq_cm_entry *entry,
				size_t max_dest_len, const void *priv_data,
				size_t priv_datalen)
{
	const struct vrb_cm_data_hdr *cm_hdr = priv_data;

	size_t datalen = MIN(max_dest_len - sizeof(*entry), cm_hdr->size);
	if (datalen)
		memcpy(entry->data, cm_hdr->data, datalen);

	return datalen;
}

static void vrb_eq_skip_rdma_cm_hdr(const void **priv_data,
						size_t *priv_data_len)
{
	size_t rdma_cm_hdr_len = sizeof(struct vrb_rdma_cm_hdr);

	if (*priv_data_len > rdma_cm_hdr_len) {
		*priv_data = (void*)((char *)*priv_data + rdma_cm_hdr_len);
		*priv_data_len -= rdma_cm_hdr_len;
	}
}

static void vrb_eq_skip_xrc_cm_data(const void **priv_data,
				       size_t *priv_data_len)
{
	const struct vrb_xrc_cm_data *cm_data = *priv_data;

	if (*priv_data_len > sizeof(*cm_data)) {
		*priv_data = (cm_data + 1);
		*priv_data_len -= sizeof(*cm_data);
	}
}

static inline void vrb_set_sidr_conn_key(struct sockaddr *addr,
					    uint16_t pep_port, bool recip,
					    struct vrb_sidr_conn_key *key)
{
	key->addr = addr;
	key->pep_port = pep_port;
	key->recip = recip;
}

static int vrb_sidr_conn_compare(struct ofi_rbmap *map,
				    void *key, void *data)
{
	struct vrb_sidr_conn_key *_key = key;
	struct vrb_xrc_ep *ep = data;
	int ret;

	assert(_key->addr->sa_family ==
	       ofi_sa_family(ep->base_ep.info_attr.dest_addr));

	/* The interface address and the passive endpoint port define
	 * the unique connection to a peer */
	switch(_key->addr->sa_family) {
	case AF_INET:
		ret = memcmp(&ofi_sin_addr(_key->addr),
			     &ofi_sin_addr(ep->base_ep.info_attr.dest_addr),
			     sizeof(ofi_sin_addr(_key->addr)));
		break;
	case AF_INET6:
		ret = memcmp(&ofi_sin6_addr(_key->addr),
			     &ofi_sin6_addr(ep->base_ep.info_attr.dest_addr),
			     sizeof(ofi_sin6_addr(_key->addr)));
		break;
	default:
		VRB_WARN(FI_LOG_EP_CTRL, "Unsuuported address format\n");
		assert(0);
		ret = -FI_EINVAL;
	}

	if (ret)
		return ret;

	if (_key->pep_port != ep->remote_pep_port)
		return _key->pep_port < ep->remote_pep_port ? -1 : 1;

	return _key->recip < ep->recip_accept ?
		-1 : _key->recip > ep->recip_accept;
}

static struct vrb_xrc_ep *
vrb_eq_get_sidr_conn(struct vrb_eq *eq, struct sockaddr *peer,
		     uint16_t pep_port, bool recip)
{
	struct ofi_rbnode *node;
	struct vrb_sidr_conn_key key;

	assert(ofi_mutex_held(&eq->event_lock));
	vrb_set_sidr_conn_key(peer, pep_port, recip, &key);
	node = ofi_rbmap_find(&eq->xrc.sidr_conn_rbmap, &key);
	if (OFI_LIKELY(!node))
		return NULL;

	return (struct vrb_xrc_ep *) node->data;
}

int vrb_eq_add_sidr_conn(struct vrb_xrc_ep *ep,
			 void *param_data, size_t param_len)
{
	int ret;
	struct vrb_sidr_conn_key key;

	assert(ofi_mutex_held(&ep->base_ep.eq->event_lock));
	assert(!ep->accept_param_data);
	assert(param_len);
	assert(ep->tgt_id && ep->tgt_id->ps == RDMA_PS_UDP);

	vrb_set_sidr_conn_key(ep->base_ep.info_attr.dest_addr,
				 ep->remote_pep_port, ep->recip_accept, &key);
	ep->accept_param_data = calloc(1, param_len);
	if (!ep->accept_param_data) {
		VRB_WARN(FI_LOG_EP_CTRL,
			   "SIDR alloc conn param memory failure\n");
		return -FI_ENOMEM;
	}
	memcpy(ep->accept_param_data, param_data, param_len);
	ep->accept_param_len = param_len;

	ret = ofi_rbmap_insert(&ep->base_ep.eq->xrc.sidr_conn_rbmap,
			       &key, (void *) ep, &ep->conn_map_node);
	assert(ret != -FI_EALREADY);
	if (OFI_UNLIKELY(ret)) {
		VRB_WARN(FI_LOG_EP_CTRL,
			   "SIDR conn map entry insert error %d\n", ret);
		free(ep->accept_param_data);
		ep->accept_param_data = NULL;
		return ret;
	}

	return FI_SUCCESS;
}

void vrb_eq_remove_sidr_conn(struct vrb_xrc_ep *ep)
{
	assert(ofi_mutex_held(&ep->base_ep.eq->event_lock));
	assert(ep->conn_map_node);

	ofi_rbmap_delete(&ep->base_ep.eq->xrc.sidr_conn_rbmap,
			 ep->conn_map_node);
	ep->conn_map_node = NULL;
	free(ep->accept_param_data);
	ep->accept_param_data = NULL;
}

static int
vrb_eq_accept_recip_conn(struct vrb_xrc_ep *ep,
			 struct fi_eq_cm_entry *entry, size_t len,
			 uint32_t *event, struct rdma_cm_event *cma_event,
			 int *acked)
{
	struct vrb_xrc_cm_data cm_data;
	int ret;

	assert(ep->conn_state == VRB_XRC_ORIG_CONNECTED);

	ret = vrb_accept_xrc(ep, VRB_RECIP_CONN, &cm_data, sizeof(cm_data));
	if (ret) {
		VRB_WARN(FI_LOG_EP_CTRL,
			   "Reciprocal XRC Accept failed %d\n", ret);
		return ret;
	}

	/* SIDR based shared reciprocal connections are complete at
	 * this point, generate the connection established event. */
	if (ep->tgt_id->ps == RDMA_PS_UDP) {
		vrb_next_xrc_conn_state(ep);
		vrb_ep_tgt_conn_done(ep);
		entry->fid = &ep->base_ep.util_ep.ep_fid.fid;
		*event = FI_CONNECTED;
		len = vrb_eq_copy_event_data(entry, len,
						ep->conn_setup->event_data,
						ep->conn_setup->event_len);
		*acked = 1;
		rdma_ack_cm_event(cma_event);
		vrb_free_xrc_conn_setup(ep, 1);

		return sizeof(*entry) + len;
	}

	/* Event is handled internally and not passed to the application */
	return -FI_EAGAIN;
}

static int
vrb_eq_xrc_connreq_event(struct vrb_eq *eq, struct fi_eq_cm_entry *entry,
			    size_t len, uint32_t *event,
			    struct rdma_cm_event *cma_event, int *acked,
			    const void **priv_data, size_t *priv_datalen)
{
	struct vrb_connreq *connreq = container_of(entry->info->handle,
						struct vrb_connreq, handle);
	struct vrb_xrc_ep *ep;
	int ret;

	/*
	 * If this is a retransmitted SIDR request for a previously accepted
	 * connection then the shared SIDR response message was lost and must
	 * be retransmitted. Note that a lost SIDR reject response message will
	 * be rejected again by the application.
	 */
	assert(entry->info->dest_addr);
	if (cma_event->id->ps == RDMA_PS_UDP) {
		ep = vrb_eq_get_sidr_conn(eq, entry->info->dest_addr,
					     connreq->xrc.port,
					     connreq->xrc.is_reciprocal);
		if (ep) {
			VRB_DBG(FI_LOG_EP_CTRL,
				  "SIDR %s request retry received\n",
				  connreq->xrc.is_reciprocal ?
				  "reciprocal" : "original");
			ret = vrb_resend_shared_accept_xrc(ep, connreq,
							      cma_event->id);
			if (ret)
				VRB_WARN(FI_LOG_EP_CTRL,
					   "SIDR accept resend failure %d\n",
					   -errno);
			rdma_destroy_id(cma_event->id);
			return -FI_EAGAIN;
		}
	}

	if (!connreq->xrc.is_reciprocal) {
		vrb_eq_skip_xrc_cm_data(priv_data, priv_datalen);
		return FI_SUCCESS;
	}

	/*
	 * Reciprocal connections are initiated and handled internally by
	 * the provider, get the endpoint that issued the original connection
	 * request.
	 */
	ep = vrb_eq_xrc_conn_tag2ep(eq, connreq->xrc.conn_tag);
	if (!ep) {
		VRB_WARN(FI_LOG_EP_CTRL,
			   "Reciprocal XRC connection tag 0x%x not found\n",
			   connreq->xrc.conn_tag);
		return -FI_EAGAIN;
	}
	ep->recip_req_received = 1;

	assert(ep->conn_state == VRB_XRC_ORIG_CONNECTED ||
	       ep->conn_state == VRB_XRC_ORIG_CONNECTING);

	ep->tgt_id = connreq->id;
	ep->tgt_id->context = &ep->base_ep.util_ep.ep_fid.fid;
	ep->base_ep.info_attr.handle = entry->info->handle;

	ret = rdma_migrate_id(ep->tgt_id, ep->base_ep.eq->channel);
	if (ret) {
		VRB_WARN(FI_LOG_EP_CTRL, "Could not migrate CM ID\n");
		goto send_reject;
	}

	/* If the initial connection has completed proceed with accepting
	 * the reciprocal; otherwise wait until it has before proceeding */
	if (ep->conn_state == VRB_XRC_ORIG_CONNECTED)
		return vrb_eq_accept_recip_conn(ep, entry, len, event,
						   cma_event, acked);

	return -FI_EAGAIN;

send_reject:
	vrb_prof_func_start("rdma_reject");
	if (rdma_reject(connreq->id, *priv_data, *priv_datalen))
		VRB_WARN(FI_LOG_EP_CTRL, "rdma_reject %d\n", -errno);
	vrb_prof_func_end("rdma_reject");
	if (rdma_destroy_id(connreq->id))
		VRB_WARN(FI_LOG_EP_CTRL, "rdma_destroy_id %d\n", -errno);

	ep->base_ep.info_attr.handle = NULL;
	ep->tgt_id = NULL;
	ep->recip_req_received = 0;
	connreq->id = NULL;

	return -FI_EAGAIN;
}

static void
vrb_eq_xrc_establish(struct rdma_cm_event *cma_event)
{
	/* For newer rdma-core, active side  must complete the
	 * connect if rdma_cm is not managing the QP */
	if (cma_event->event == RDMA_CM_EVENT_CONNECT_RESPONSE &&
	    !cma_event->id->qp)
		rdma_establish(cma_event->id);
}

static int
vrb_eq_xrc_conn_event(struct vrb_xrc_ep *ep,
			 struct rdma_cm_event *cma_event, int *acked,
			 struct fi_eq_cm_entry *entry, size_t len,
			 uint32_t *event)
{
	struct vrb_xrc_conn_info xrc_info;
	struct vrb_xrc_cm_data cm_data = { 0 };
	const void *priv_data = cma_event->param.conn.private_data;
	size_t priv_datalen = cma_event->param.conn.private_data_len;
	int ret;

	VRB_DBG(FI_LOG_EP_CTRL, "EP %p INITIAL CONNECTION DONE state %d, ps %d\n",
		  ep, ep->conn_state, cma_event->id->ps);
	vrb_next_xrc_conn_state(ep);

	/*
	 * Original application initiated connect is done, if the passive
	 * side of that connection initiate the reciprocal connection request
	 * to create bidirectional connectivity.
	 */
	if (priv_data) {
		ret = vrb_eq_set_xrc_info(cma_event, &xrc_info);
		if (ret) {
			vrb_prev_xrc_conn_state(ep);
			rdma_disconnect(ep->base_ep.id);
			goto err;
		}
		ep->peer_srqn = xrc_info.peer_srqn;
		vrb_eq_skip_xrc_cm_data(&priv_data, &priv_datalen);
		vrb_save_priv_data(ep, priv_data, priv_datalen);
		vrb_ep_ini_conn_done(ep, xrc_info.conn_param.qp_num);
		vrb_eq_xrc_establish(cma_event);

		/* If we have received the reciprocal connect request,
		 * process it now */
		if (ep->recip_req_received)
			return vrb_eq_accept_recip_conn(ep, entry,
							   len, event,
							   cma_event, acked);
	} else {
		vrb_ep_tgt_conn_done(ep);
		ret = vrb_connect_xrc(ep, NULL, VRB_RECIP_CONN, &cm_data,
					 sizeof(cm_data));
		if (ret) {
			vrb_prev_xrc_conn_state(ep);
			ep->tgt_id->qp = NULL;
			rdma_disconnect(ep->tgt_id);
			goto err;
		}
	}
err:
	entry->info = NULL;
	/* Event is handled internally and not passed to the application */
	return -FI_EAGAIN;
}

static ssize_t
vrb_eq_xrc_recip_conn_event(struct vrb_eq *eq,
			       struct vrb_xrc_ep *ep,
			       struct rdma_cm_event *cma_event,
			       struct fi_eq_cm_entry *entry, size_t len)
{
	fid_t fid = cma_event->id->context;
	struct vrb_xrc_conn_info xrc_info;
	int ret;

	vrb_next_xrc_conn_state(ep);
	VRB_DBG(FI_LOG_EP_CTRL, "EP %p RECIPROCAL CONNECTION DONE state %d\n",
		  ep, ep->conn_state);

	/* If this is the reciprocal active side notification */
	if (cma_event->param.conn.private_data) {
		ret = vrb_eq_set_xrc_info(cma_event, &xrc_info);
		if (ret) {
			VRB_WARN(FI_LOG_EP_CTRL,
				   "Reciprocal connection protocol mismatch\n");
			eq->err.err = -ret;
			eq->err.prov_errno = ret;
			eq->err.fid = fid;
			return -FI_EAVAIL;
		}

		ep->peer_srqn = xrc_info.peer_srqn;
		vrb_ep_ini_conn_done(ep, xrc_info.conn_param.qp_num);
		vrb_eq_xrc_establish(cma_event);
	} else {
		vrb_ep_tgt_conn_done(ep);
	}

	/* The internal reciprocal XRC connection has completed. Return the
	 * CONNECTED event application data associated with the original
	 * connection. */
	entry->fid = fid;
	len = vrb_eq_copy_event_data(entry, len,
					ep->conn_setup->event_data,
					ep->conn_setup->event_len);
	entry->info = NULL;
	return sizeof(*entry) + len;
}

static int
vrb_eq_xrc_rej_event(struct vrb_eq *eq, struct rdma_cm_event *cma_event)
{
	struct vrb_xrc_ep *ep;
	fid_t fid = cma_event->id->context;
	struct vrb_xrc_conn_info xrc_info;
	enum vrb_xrc_ep_conn_state state;

	assert(ofi_mutex_held(&eq->event_lock));
	ep = container_of(fid, struct vrb_xrc_ep, base_ep.util_ep.ep_fid);
	if (ep->magic != VERBS_XRC_EP_MAGIC) {
		VRB_WARN(FI_LOG_EP_CTRL,
			   "CM ID context not valid\n");
		return -FI_EAGAIN;
	}

	state = ep->conn_state;
	if (ep->base_ep.id != cma_event->id ||
	    (state != VRB_XRC_ORIG_CONNECTING &&
	     state != VRB_XRC_RECIP_CONNECTING)) {
		VRB_WARN(FI_LOG_EP_CTRL,
			   "Stale/invalid CM reject %d received\n", cma_event->status);
		return -FI_EAGAIN;
	}

	/* If reject comes from remote provider peer */
	if (cma_event->status == VRB_CM_REJ_CONSUMER_DEFINED ||
	    cma_event->status == VRB_CM_REJ_SIDR_CONSUMER_DEFINED) {
		if (cma_event->param.conn.private_data_len &&
		    vrb_eq_set_xrc_info(cma_event, &xrc_info)) {
			VRB_WARN(FI_LOG_EP_CTRL,
				   "CM REJ private data not valid\n");
			return -FI_EAGAIN;
		}

		vrb_ep_ini_conn_rejected(ep);
		return FI_SUCCESS;
	}

	VRB_WARN(FI_LOG_EP_CTRL, "Non-application generated CM Reject %d\n",
		   cma_event->status);
	if (cma_event->param.conn.private_data_len)
		VRB_WARN(FI_LOG_EP_CTRL, "Unexpected CM Reject priv_data\n");

	vrb_ep_ini_conn_rejected(ep);

	return state == VRB_XRC_ORIG_CONNECTING ? FI_SUCCESS : -FI_EAGAIN;
}

static int
vrb_eq_xrc_connect_retry(struct vrb_xrc_ep *ep,
			 struct rdma_cm_event *cma_event, int *acked)
{
	assert(ofi_mutex_held(&ep->base_ep.eq->event_lock));

	if (ep->base_ep.info_attr.src_addr)
		ofi_straddr_dbg(&vrb_prov, FI_LOG_EP_CTRL,
				"Connect retry src ",
				ep->base_ep.info_attr.src_addr);
	if (ep->base_ep.info_attr.dest_addr)
		ofi_straddr_dbg(&vrb_prov, FI_LOG_EP_CTRL,
				"Connect retry dest ",
				ep->base_ep.info_attr.dest_addr);

	*acked = 1;
	rdma_ack_cm_event(cma_event);
	rdma_destroy_id(ep->base_ep.id);
	ep->base_ep.id = NULL;
	vrb_eq_clear_xrc_conn_tag(ep);
	ep->conn_setup->retry_count++;
	return vrb_connect_xrc(ep, NULL, ep->conn_setup->pending_recip,
			       ep->conn_setup->pending_param,
			       ep->conn_setup->pending_paramlen);
}

static int
vrb_eq_xrc_cm_err_event(struct vrb_eq *eq,
                        struct rdma_cm_event *cma_event, int *acked)
{
	struct vrb_xrc_ep *ep;
	fid_t fid = cma_event->id->context;
	int ret;

	assert(ofi_mutex_held(&eq->event_lock));
	ep = container_of(fid, struct vrb_xrc_ep, base_ep.util_ep.ep_fid);
	if (ep->magic != VERBS_XRC_EP_MAGIC) {
		VRB_WARN(FI_LOG_EP_CTRL, "CM ID context invalid\n");
		return -FI_EAGAIN;
	}

	/* Connect errors can be reported on active or passive side, all other
	 * errors considered are reported on the active side only */
	if ((ep->base_ep.id != cma_event->id) &&
	    (cma_event->event == RDMA_CM_EVENT_CONNECT_ERROR &&
	     ep->tgt_id != cma_event->id)) {
		VRB_WARN(FI_LOG_EP_CTRL, "CM error not valid for EP\n");
		return -FI_EAGAIN;
	}

	if (ep->base_ep.id == cma_event->id) {
		vrb_put_shared_ini_conn(ep);

		/* Active side connect errors are retried */
		if (ep->conn_setup && (ep->conn_setup->retry_count <
				       VRB_MAX_XRC_CONNECT_RETRIES)) {
			ret = vrb_eq_xrc_connect_retry(ep, cma_event, acked);
			if (!ret)
				return -FI_EAGAIN;
		}
	}

	VRB_WARN(FI_LOG_EP_CTRL, "CM error event %s, status %d\n",
		   rdma_event_str(cma_event->event), cma_event->status);
	if (ep->base_ep.info_attr.src_addr)
		ofi_straddr_log(&vrb_prov, FI_LOG_WARN, FI_LOG_EP_CTRL,
				"Src ", ep->base_ep.info_attr.src_addr);
	if (ep->base_ep.info_attr.dest_addr)
		ofi_straddr_log(&vrb_prov, FI_LOG_WARN, FI_LOG_EP_CTRL,
				"Dest ", ep->base_ep.info_attr.dest_addr);
        ep->conn_state = VRB_XRC_ERROR;
        return FI_SUCCESS;
}

static ssize_t
vrb_eq_xrc_connected_event(struct vrb_eq *eq,
			   struct rdma_cm_event *cma_event, int *acked,
			   struct fi_eq_cm_entry *entry, size_t len,
			   uint32_t *event)
{
	struct vrb_xrc_ep *ep;
	fid_t fid = cma_event->id->context;
	ssize_t ret;

	ep = container_of(fid, struct vrb_xrc_ep, base_ep.util_ep.ep_fid);

	assert(ofi_mutex_held(&eq->event_lock));
	assert(ep->conn_state == VRB_XRC_ORIG_CONNECTING ||
	       ep->conn_state == VRB_XRC_RECIP_CONNECTING);

	if (ep->conn_state == VRB_XRC_ORIG_CONNECTING)
		return vrb_eq_xrc_conn_event(ep, cma_event, acked,
						entry, len, event);

	ret = vrb_eq_xrc_recip_conn_event(eq, ep, cma_event, entry, len);

	/* Bidirectional connection setup is complete, release RDMA CM ID
	 * resources. */
	*acked = 1;
	rdma_ack_cm_event(cma_event);
	vrb_free_xrc_conn_setup(ep, 1);

	return ret;
}

static void
vrb_eq_xrc_timewait_event(struct vrb_eq *eq,
			  struct rdma_cm_event *cma_event, int *acked)
{
	fid_t fid = cma_event->id->context;
	struct vrb_xrc_ep *ep = container_of(fid, struct vrb_xrc_ep,
						base_ep.util_ep.ep_fid);

	assert(ofi_mutex_held(&eq->event_lock));
	assert(ep->magic == VERBS_XRC_EP_MAGIC);
	assert(ep->conn_setup);

	if (cma_event->id == ep->tgt_id) {
		*acked = 1;
		rdma_ack_cm_event(cma_event);
		rdma_destroy_id(ep->tgt_id);
		ep->tgt_id = NULL;
	} else if (cma_event->id == ep->base_ep.id) {
		*acked = 1;
		rdma_ack_cm_event(cma_event);
		rdma_destroy_id(ep->base_ep.id);
		ep->base_ep.id = NULL;
	}
	if (!ep->base_ep.id && !ep->tgt_id)
		vrb_free_xrc_conn_setup(ep, 0);
}

static inline void
vrb_eq_xrc_disconnect_event(struct vrb_eq *eq,
			       struct rdma_cm_event *cma_event, int *acked)
{
	fid_t fid = cma_event->id->context;
	struct vrb_xrc_ep *ep = container_of(fid, struct vrb_xrc_ep,
					     base_ep.util_ep.ep_fid);

	assert(ofi_mutex_held(&eq->event_lock));
	assert(ep->magic == VERBS_XRC_EP_MAGIC);

	if (ep->conn_setup && cma_event->id == ep->base_ep.id) {
		*acked = 1;
		rdma_ack_cm_event(cma_event);
		rdma_disconnect(ep->base_ep.id);
	}
}

static int
vrb_eq_addr_resolved_event(struct vrb_ep *ep)
{
	struct vrb_recv_wr *wr;
	struct slist_entry *entry;
	struct ibv_qp_init_attr attr = { 0 };
	int ret;
	//struct vrb_domain *domain = vrb_ep2_domain(ep);

	assert(ofi_genlock_held(&vrb_ep2_progress(ep)->ep_lock));
	assert(ep->state == VRB_RESOLVE_ADDR);
	if (ep->util_ep.type == FI_EP_MSG) {
		vrb_msg_ep_get_qp_attr(ep, &attr);

		/* Client-side QP creation */
		vrb_prof_func_start("rdma_create_qp");
		if (rdma_create_qp(ep->id, vrb_ep2_domain(ep)->pd, &attr)) {
			ep->state = VRB_DISCONNECTED;
			ret = -errno;
			VRB_WARN(FI_LOG_EP_CTRL,
				 "rdma_create_qp failed: %d\n", -ret);
			return ret;
		}
		vrb_prof_func_end("rdma_create_qp");
		if (ep->profile)
			vrb_prof_cntr_inc(ep->profile, FI_VAR_MSG_QUEUE_CNT);

		/* Allow shared XRC INI QP not controlled by RDMA CM
		 * to share same post functions as RC QP. */
		ep->ibv_qp = ep->id->qp;
	}

	assert(ep->ibv_qp);
	while (!slist_empty(&ep->prepost_wr_list)) {
		entry = ep->prepost_wr_list.head;
		wr = container_of(entry, struct vrb_recv_wr, entry);

		ret = vrb_post_recv_internal(ep, &wr->wr);
		if (ret) {
			VRB_WARN(FI_LOG_EP_CTRL,
			         "Failed to post receive buffers: %d\n", -ret);

			return ret;
		}
		vrb_free_recv_wr(vrb_ep2_progress(ep), wr);
		slist_remove_head(&ep->prepost_wr_list);
	}

	ep->state = VRB_RESOLVE_ROUTE;
	vrb_prof_func_start("rdma_resolve_route");
	if (rdma_resolve_route(ep->id, VERBS_RESOLVE_TIMEOUT)) {
		ep->state = VRB_DISCONNECTED;
		ret = -errno;
		VRB_WARN(FI_LOG_EP_CTRL,
			"rdma_resolve_route failed: %d\n",
			-ret);
		return ret;
	}
	vrb_prof_func_end("rdma_resolve_route");

	return -FI_EAGAIN;
}

static ssize_t
vrb_eq_cm_process_event(struct vrb_eq *eq,
	struct rdma_cm_event *cma_event, uint32_t *event,
	struct fi_eq_cm_entry *entry, size_t len)
{
	const struct vrb_cm_data_hdr *cm_hdr;
	size_t datalen = 0;
	size_t priv_datalen = cma_event->param.conn.private_data_len;
	const void *priv_data = cma_event->param.conn.private_data;
	int ret, acked = 0;;
	fid_t fid = cma_event->id->context;
	struct vrb_pep *pep =
		container_of(fid, struct vrb_pep, pep_fid);
	struct vrb_ep *ep;
	struct vrb_xrc_ep *xrc_ep;

	assert(ofi_mutex_held(&eq->event_lock));
	switch (cma_event->event) {
	case RDMA_CM_EVENT_ADDR_RESOLVED:
		ep = container_of(fid, struct vrb_ep, util_ep.ep_fid);
		if (ep->profile)
			vrb_prof_set_st_time(ep->profile, (ofi_gettime_ns()),
					VRB_RESOLVE_ADDR);

		ofi_genlock_lock(&vrb_ep2_progress(ep)->ep_lock);
		ret = vrb_eq_addr_resolved_event(ep);
		ofi_genlock_unlock(&vrb_ep2_progress(ep)->ep_lock);
		if (ret != -FI_EAGAIN) {
			eq->err.err = -ret;
			eq->err.prov_errno = ret;
			goto err;
		}
		goto ack;

	case RDMA_CM_EVENT_ROUTE_RESOLVED:
		ep = container_of(fid, struct vrb_ep, util_ep.ep_fid);
		if (ep->profile)
			vrb_prof_set_st_time(ep->profile, (ofi_gettime_ns()),
					VRB_RESOLVE_ROUTE);
		ofi_genlock_lock(&vrb_ep2_progress(ep)->ep_lock);
		assert(ep->state == VRB_RESOLVE_ROUTE);
		ep->state = VRB_CONNECTING;

		if (cma_event->id->route.addr.src_addr.sa_family != AF_IB) {
			vrb_eq_skip_rdma_cm_hdr((const void **)&ep->conn_param.private_data,
						(size_t *)&ep->conn_param.private_data_len);
		} else {
			vrb_msg_ep_prepare_rdma_cm_hdr(ep->cm_priv_data, ep->id);
		}
		vrb_prof_func_start("rdma_connect");
		ret = rdma_connect(ep->id, &ep->conn_param);
		vrb_prof_func_end("rdma_connect");
		if (!ret && ep->profile)
			vrb_prof_cntr_inc(ep->profile, FI_VAR_CONN_REQUEST);

		if (ret) {
			ep->state = VRB_DISCONNECTED;
			ret = -errno;
			FI_WARN(&vrb_prov, FI_LOG_EP_CTRL,
				"rdma_connect failed: %s (%d)\n",
				strerror(-ret), -ret);
			if (vrb_is_xrc_ep(ep)) {
				xrc_ep = container_of(fid, struct vrb_xrc_ep,
						      base_ep.util_ep.ep_fid);
				vrb_put_shared_ini_conn(xrc_ep);
			}
		} else {
			ret = -FI_EAGAIN;
		}
		ofi_genlock_unlock(&vrb_ep2_progress(ep)->ep_lock);
		if (ret != -FI_EAGAIN) {
			eq->err.err = -ret;
			eq->err.prov_errno = ret;
			goto err;
		}
		goto ack;
	case RDMA_CM_EVENT_CONNECT_REQUEST:
		*event = FI_CONNREQ;
		ret = vrb_eq_cm_getinfo(cma_event, pep->info, &entry->info);
		if (ret) {
			VRB_WARN(FI_LOG_EP_CTRL,
				   "CM getinfo error %d\n", ret);
			rdma_destroy_id(cma_event->id);
			eq->err.err = -ret;
			eq->err.prov_errno = ret;
			goto err;
		}

		if (vrb_is_xrc_info(entry->info)) {
			ret = vrb_eq_xrc_connreq_event(eq, entry, len, event,
							  cma_event, &acked,
							  &priv_data, &priv_datalen);
			if (ret == -FI_EAGAIN) {
				fi_freeinfo(entry->info);
				entry->info = NULL;
				goto ack;
			}
			if (*event == FI_CONNECTED)
				goto ack;
		} else if (cma_event->id->route.addr.src_addr.sa_family == AF_IB) {
			vrb_eq_skip_rdma_cm_hdr(&priv_data, &priv_datalen);
		}
		break;
	case RDMA_CM_EVENT_CONNECT_RESPONSE:
	case RDMA_CM_EVENT_ESTABLISHED:
		*event = FI_CONNECTED;
		ep = container_of(fid, struct vrb_ep, util_ep.ep_fid);
		if (ep->profile) {
			vrb_prof_set_st_time(ep->profile, (ofi_gettime_ns()),
					VRB_CONNECTED);
			vrb_prof_cntr_inc(ep->profile,
					FI_VAR_CONNECTION_CNT);
		}
		if (cma_event->id->qp &&
		    cma_event->id->qp->context->device->transport_type !=
		    IBV_TRANSPORT_IWARP) {
			vrb_set_rnr_timer(cma_event->id->qp);
		}
		if (vrb_is_xrc_ep(ep)) {
			ret = vrb_eq_xrc_connected_event(eq, cma_event,
							    &acked, entry, len,
							    event);
			goto ack;
		}
		ofi_genlock_lock(&vrb_ep2_progress(ep)->ep_lock);
		assert(ep->state == VRB_CONNECTING || ep->state == VRB_ACCEPTING);
		ep->state = VRB_CONNECTED;
		ofi_genlock_unlock(&vrb_ep2_progress(ep)->ep_lock);
		entry->info = NULL;
		break;
	case RDMA_CM_EVENT_DISCONNECTED:
		ep = container_of(fid, struct vrb_ep, util_ep.ep_fid);
		if (ep->profile)
			vrb_prof_set_st_time(ep->profile, (ofi_gettime_ns()),
                                            VRB_DISCONNECTED);
		ofi_genlock_lock(&vrb_ep2_progress(ep)->ep_lock);
		if (ep->state == VRB_DISCONNECTED) {
			/* If we saw a transfer error, we already generated
			 * a shutdown event.
			 */
			ret = -FI_EAGAIN;
			ofi_genlock_unlock(&vrb_ep2_progress(ep)->ep_lock);
			goto ack;
		}
		ep->state = VRB_DISCONNECTED;
		ofi_genlock_unlock(&vrb_ep2_progress(ep)->ep_lock);
		if (vrb_is_xrc_ep(ep)) {
			vrb_eq_xrc_disconnect_event(eq, cma_event, &acked);
			ret = -FI_EAGAIN;
			goto ack;
		}
		*event = FI_SHUTDOWN;
		entry->info = NULL;
		break;
	case RDMA_CM_EVENT_TIMEWAIT_EXIT:
		ep = container_of(fid, struct vrb_ep, util_ep.ep_fid);
		if (vrb_is_xrc_ep(ep))
			vrb_eq_xrc_timewait_event(eq, cma_event, &acked);
		ret = -FI_EAGAIN;
		goto ack;
	case RDMA_CM_EVENT_ADDR_ERROR:
	case RDMA_CM_EVENT_ROUTE_ERROR:
	case RDMA_CM_EVENT_CONNECT_ERROR:
	case RDMA_CM_EVENT_UNREACHABLE:
		ep = container_of(fid, struct vrb_ep, util_ep.ep_fid);
		ofi_genlock_lock(&vrb_ep2_progress(ep)->ep_lock);
		assert(ep->state != VRB_DISCONNECTED);
		ep->state = VRB_DISCONNECTED;
		ofi_genlock_unlock(&vrb_ep2_progress(ep)->ep_lock);
		if (vrb_is_xrc_ep(ep)) {
			/* SIDR Reject is reported as UNREACHABLE unless
			 * status is negative */
			if (cma_event->id->ps == RDMA_PS_UDP &&
			    (cma_event->event == RDMA_CM_EVENT_UNREACHABLE &&
			     cma_event->status >= 0))
				goto xrc_shared_reject;

			ret = vrb_eq_xrc_cm_err_event(eq, cma_event, &acked);
			if (ret == -FI_EAGAIN)
				goto ack;

			*event = FI_SHUTDOWN;
			entry->info = NULL;
			break;
		}
		eq->err.err = ETIMEDOUT;
		eq->err.prov_errno = -cma_event->status;
		if (eq->err.err_data) {
			free(eq->err.err_data);
			eq->err.err_data = NULL;
			eq->err.err_data_size = 0;
		}
		goto err;
	case RDMA_CM_EVENT_REJECTED:
		ep = container_of(fid, struct vrb_ep, util_ep.ep_fid);
		ofi_genlock_lock(&vrb_ep2_progress(ep)->ep_lock);
		assert(ep->state != VRB_DISCONNECTED);
		ep->state = VRB_DISCONNECTED;
		ofi_genlock_unlock(&vrb_ep2_progress(ep)->ep_lock);
		if (vrb_is_xrc_ep(ep)) {
xrc_shared_reject:
			ret = vrb_eq_xrc_rej_event(eq, cma_event);
			if (ret == -FI_EAGAIN)
				goto ack;
			vrb_eq_skip_xrc_cm_data(&priv_data, &priv_datalen);
		}
		eq->err.err = ECONNREFUSED;
		eq->err.prov_errno = -cma_event->status;
		if (eq->err.err_data) {
			free(eq->err.err_data);
			eq->err.err_data = NULL;
			eq->err.err_data_size = 0;
		}
		if (priv_datalen) {
			cm_hdr = priv_data;
			eq->err.err_data = calloc(1, cm_hdr->size);
			assert(eq->err.err_data);
			memcpy(eq->err.err_data, cm_hdr->data,
			       cm_hdr->size);
			eq->err.err_data_size = cm_hdr->size;
		}
		goto err;
	case RDMA_CM_EVENT_DEVICE_REMOVAL:
		eq->err.err = ENODEV;
		goto err;
	case RDMA_CM_EVENT_ADDR_CHANGE:
		eq->err.err = EADDRNOTAVAIL;
		goto err;
	default:
		VRB_WARN(FI_LOG_EP_CTRL, "unknown rdmacm event received: %d\n",
			   cma_event->event);
		ret = -FI_EAGAIN;
		goto ack;
	}

	entry->fid = fid;

	/* rdmacm has no way to track how much data is sent by peer */
	if (priv_datalen)
		datalen = vrb_eq_copy_event_data(entry, len, priv_data,
						 priv_datalen);
	if (!acked)
		rdma_ack_cm_event(cma_event);
	return sizeof(*entry) + datalen;
err:
	ret = -FI_EAVAIL;
	eq->err.fid = fid;
ack:
	if (!acked)
		rdma_ack_cm_event(cma_event);
	return ret;
}

int vrb_eq_trywait(struct vrb_eq *eq)
{
	int ret;
	ofi_mutex_lock(&eq->lock);
	ret = dlistfd_empty(&eq->list_head);
	ofi_mutex_unlock(&eq->lock);
	return ret ? 0 : -FI_EAGAIN;
}

int vrb_eq_match_event(struct dlist_entry *item, const void *arg)
{
	struct vrb_eq_entry *entry;
	const struct fid *fid = arg;

	entry = container_of(item, struct vrb_eq_entry, item);
	switch (entry->event) {
	case FI_CONNREQ:
	case FI_CONNECTED:
	case FI_SHUTDOWN:
		return entry->cm_entry->fid == fid;
	case FI_MR_COMPLETE:
	case FI_AV_COMPLETE:
	case FI_JOIN_COMPLETE:
		return entry->eq_entry->fid == fid;
	default:
		return 0;
	}
}

void vrb_eq_remove_events(struct vrb_eq *eq, struct fid *fid)
{
	struct dlist_entry *item;
	struct vrb_eq_entry *entry;

	assert(ofi_mutex_held(&eq->event_lock));
	while ((item =
		dlistfd_remove_first_match(&eq->list_head,
					   vrb_eq_match_event, fid))) {
		entry = container_of(item, struct vrb_eq_entry, item);
		if (entry->event == FI_CONNREQ)
			fi_freeinfo(entry->cm_entry->info);
		free(entry);
	}
}

struct vrb_eq_entry  *
vrb_eq_alloc_entry(uint32_t event, const void *buf, size_t len)
{
	struct vrb_eq_entry *entry;

	entry = calloc(1, sizeof(struct vrb_eq_entry) + len);
	if (!entry)
		return NULL;

	entry->event = event;
	entry->len = len;
	memcpy(entry->data, buf, len);

	return entry;
}

ssize_t vrb_eq_write_event(struct vrb_eq *eq, uint32_t event,
			      const void *buf, size_t len)
{
	struct vrb_eq_entry *entry;

	entry = vrb_eq_alloc_entry(event, buf, len);
	if (!entry)
		return -FI_ENOMEM;

	ofi_mutex_lock(&eq->lock);
	dlistfd_insert_tail(&entry->item, &eq->list_head);
	ofi_mutex_unlock(&eq->lock);

	return len;
}

static ssize_t vrb_eq_write(struct fid_eq *eq_fid, uint32_t event,
			       const void *buf, size_t len, uint64_t flags)
{
	struct vrb_eq *eq;

	eq = container_of(eq_fid, struct vrb_eq, eq_fid.fid);
	if (!(eq->flags & FI_WRITE))
		return -FI_EINVAL;

	return vrb_eq_write_event(eq, event, buf, len);
}

static size_t vrb_eq_read_event(struct vrb_eq *eq, uint32_t *event,
		void *buf, size_t len, uint64_t flags)
{
	struct vrb_eq_entry *entry;
	ssize_t ret = 0;

	ofi_mutex_lock(&eq->lock);

	if (eq->err.err) {
		ret = -FI_EAVAIL;
		goto out;
	}

	if (dlistfd_empty(&eq->list_head))
		goto out;

	entry = container_of(eq->list_head.list.next, struct vrb_eq_entry, item);
	if (entry->len > len) {
		ret = -FI_ETOOSMALL;
		goto out;
	}

	ret = entry->len;
	*event = entry->event;
	memcpy(buf, entry->data, entry->len);

	if (!(flags & FI_PEEK)) {
		dlistfd_remove(eq->list_head.list.next, &eq->list_head);
		free(entry);
	}

out:
	ofi_mutex_unlock(&eq->lock);
	return ret;
}

static void vrb_eq_process_async_events(struct vrb_eq *eq)
{
	int ret;
	struct vrb_domain *domain;
	struct ibv_async_event async_event;

	ofi_mutex_lock(&eq->fab->util_fabric.lock);
	dlist_foreach_container(&eq->fab->util_fabric.domain_list,
				struct vrb_domain, domain,
				util_domain.list_entry) {
		do {
			ret = ibv_get_async_event(domain->verbs, &async_event);
			if (!ret) {
				VRB_WARN(FI_LOG_DOMAIN, "Async event for %s: %s\n",
					 eq->fab->info->domain_attr->name,
					 ibv_event_type_str(async_event.event_type));
				ibv_ack_async_event(&async_event);
			}
		} while (!ret);
	}
	ofi_mutex_unlock(&eq->fab->util_fabric.lock);
}

static ssize_t
vrb_eq_read(struct fid_eq *eq_fid, uint32_t *event,
	    void *buf, size_t len, uint64_t flags)
{
	struct vrb_eq *eq;
	struct rdma_cm_event *cma_event;
	ssize_t ret;

	if (len < sizeof(struct fi_eq_cm_entry))
		return -FI_ETOOSMALL;

	vrb_prof_func_start(__func__);

	eq = container_of(eq_fid, struct vrb_eq, eq_fid.fid);
	assert(eq->channel);

	ret = vrb_eq_read_event(eq, event, buf, len, flags);
	if (ret)
		return ret;

	/* Skip events that are handled internally (e.g. XRC CM events). */
	do {
		ofi_mutex_lock(&eq->event_lock);
		vrb_prof_func_start("rdma_get_cm_event");
		ret = rdma_get_cm_event(eq->channel, &cma_event);
		vrb_prof_func_end("rdma_get_cm_event");
		if (ret) {
			ofi_mutex_unlock(&eq->event_lock);
			ret = -errno;
			goto out;
		}
		vrb_prof_func_start("vrb_eq_cm_process_event");
		ret = vrb_eq_cm_process_event(eq, cma_event, event, buf, len);
		vrb_prof_func_end("vrb_eq_cm_process_event");
		ofi_mutex_unlock(&eq->event_lock);

	} while (ret == -FI_EAGAIN);

	if (ret > 0 && flags & FI_PEEK)
		ret = vrb_eq_write_event(eq, *event, buf, ret);
out:
	if (ret <= 0)
		vrb_eq_process_async_events(eq);
	vrb_prof_func_end(__func__);
	return ret;
}

static ssize_t
vrb_eq_sread(struct fid_eq *eq_fid, uint32_t *event,
		void *buf, size_t len, int timeout, uint64_t flags)
{
	struct vrb_eq *eq;
	struct ofi_epollfds_event fdevent;
	ssize_t ret;

	eq = container_of(eq_fid, struct vrb_eq, eq_fid.fid);

	while (1) {
		ret = vrb_eq_read(eq_fid, event, buf, len, flags);
		if (ret && (ret != -FI_EAGAIN))
			return ret;

		ret = ofi_epoll_wait(eq->epollfd, &fdevent, 1, timeout);
		if (ret == 0)
			return -FI_EAGAIN;
		else if (ret < 0)
			return -errno;
	}
}

static const char *
vrb_eq_strerror(struct fid_eq *eq, int prov_errno, const void *err_data,
		   char *buf, size_t len)
{
	if (buf && len)
		strncpy(buf, strerror(prov_errno), len);
	return strerror(prov_errno);
}

static struct fi_ops_eq vrb_eq_ops = {
	.size = sizeof(struct fi_ops_eq),
	.read = vrb_eq_read,
	.readerr = vrb_eq_readerr,
	.write = vrb_eq_write,
	.sread = vrb_eq_sread,
	.strerror = vrb_eq_strerror
};

static int vrb_eq_control(fid_t fid, int command, void *arg)
{
	struct fi_wait_pollfd *pollfd;
	struct vrb_eq *eq;
	int ret;

	eq = container_of(fid, struct vrb_eq, eq_fid.fid);
	switch (command) {
	case FI_GETWAIT:
#if defined (HAVE_EPOLL) || defined(HAVE_KQUEUE)
		if (eq->wait_obj == FI_WAIT_FD) {
			*(int *) arg = eq->epollfd;
			return 0;
		}

		pollfd = arg;
		if (pollfd->nfds >= 1) {
			pollfd->fd[0].fd = eq->epollfd;
			pollfd->fd[0].events = POLLIN;
			ret = 0;
		} else {
			ret = -FI_ETOOSMALL;
		}
		pollfd->change_index = 1;
		pollfd->nfds = 1;
#else
		/* We expect verbs to only run on systems with epoll/kqueue */
		return -FI_ENOSYS;
#endif
		break;
	case FI_GETWAITOBJ:
		*(enum fi_wait_obj *) arg = eq->wait_obj;
		ret = 0;
		break;
	default:
		ret = -FI_ENOSYS;
		break;
	}

	return ret;
}

static int vrb_eq_close(fid_t fid)
{
	struct vrb_eq *eq;
	struct vrb_eq_entry *entry;

	eq = container_of(fid, struct vrb_eq, eq_fid.fid);
	/* TODO: use util code, if possible */
	if (ofi_atomic_get32(&eq->ref))
		return -FI_EBUSY;

	if (!ofi_rbmap_empty(&eq->xrc.sidr_conn_rbmap))
		VRB_WARN(FI_LOG_EP_CTRL, "SIDR connection RBmap not empty\n");

	free(eq->err.err_data);

	if (eq->channel)
		rdma_destroy_event_channel(eq->channel);

	ofi_epoll_close(eq->epollfd);

	while (!dlistfd_empty(&eq->list_head)) {
		entry = container_of(eq->list_head.list.next,
				     struct vrb_eq_entry, item);
		dlistfd_remove(eq->list_head.list.next, &eq->list_head);
		free(entry);
	}

	dlistfd_head_free(&eq->list_head);

	ofi_rbmap_cleanup(&eq->xrc.sidr_conn_rbmap);
	ofi_idx_reset(eq->xrc.conn_key_map);
	free(eq->xrc.conn_key_map);
	ofi_mutex_destroy(&eq->event_lock);
	ofi_mutex_destroy(&eq->lock);
	free(eq);

	return 0;
}

static struct fi_ops vrb_eq_fi_ops = {
	.size = sizeof(struct fi_ops),
	.close = vrb_eq_close,
	.bind = fi_no_bind,
	.control = vrb_eq_control,
	.ops_open = fi_no_ops_open,
};

int vrb_eq_open(struct fid_fabric *fabric, struct fi_eq_attr *attr,
		   struct fid_eq **eq, void *context)
{
	struct vrb_eq *_eq;
	int ret;

	_eq = calloc(1, sizeof *_eq);
	if (!_eq)
		return -ENOMEM;

	switch (attr->wait_obj) {
	case FI_WAIT_NONE:
	case FI_WAIT_UNSPEC:
	case FI_WAIT_FD:
		_eq->wait_obj = FI_WAIT_FD;
		break;
	case FI_WAIT_POLLFD:
		_eq->wait_obj = FI_WAIT_POLLFD;
		break;
	default:
		ret = -FI_ENOSYS;
		goto err0;
	}

	_eq->fab = container_of(fabric, struct vrb_fabric,
				util_fabric.fabric_fid);

	ofi_key_idx_init(&_eq->xrc.conn_key_idx, VERBS_CONN_TAG_INDEX_BITS);
	_eq->xrc.conn_key_map = calloc(1, sizeof(*_eq->xrc.conn_key_map));
	if (!_eq->xrc.conn_key_map) {
		ret = -ENOMEM;
		goto err0;
	}
	ofi_rbmap_init(&_eq->xrc.sidr_conn_rbmap, vrb_sidr_conn_compare);

	ofi_mutex_init(&_eq->lock);
	ofi_mutex_init(&_eq->event_lock);
	ret = dlistfd_head_init(&_eq->list_head);
	if (ret) {
		VRB_INFO(FI_LOG_EQ, "Unable to initialize dlistfd\n");
		goto err1;
	}

	ret = ofi_epoll_create(&_eq->epollfd);
	if (ret)
		goto err2;

	if (ofi_epoll_add(_eq->epollfd, _eq->list_head.signal.fd[FI_READ_FD],
			  OFI_EPOLL_IN, NULL)) {
		ret = -errno;
		goto err3;
	}

	_eq->channel = rdma_create_event_channel();
	if (!_eq->channel) {
		ret = -errno;
		goto err3;
	}

	ret = fi_fd_nonblock(_eq->channel->fd);
	if (ret)
		goto err4;

	if (ofi_epoll_add(_eq->epollfd, _eq->channel->fd, OFI_EPOLL_IN, NULL)) {
		ret = -errno;
		goto err4;
	}

	_eq->flags = attr->flags;
	_eq->eq_fid.fid.fclass = FI_CLASS_EQ;
	_eq->eq_fid.fid.context = context;
	_eq->eq_fid.fid.ops = &vrb_eq_fi_ops;
	_eq->eq_fid.ops = &vrb_eq_ops;

	ofi_atomic_initialize32(&_eq->ref, 0);

	*eq = &_eq->eq_fid;
	return 0;
err4:
	if (_eq->channel)
		rdma_destroy_event_channel(_eq->channel);
err3:
	ofi_epoll_close(_eq->epollfd);
err2:
	dlistfd_head_free(&_eq->list_head);
err1:
	ofi_mutex_destroy(&_eq->event_lock);
	ofi_mutex_destroy(&_eq->lock);
	free(_eq->xrc.conn_key_map);
err0:
	free(_eq);
	return ret;
}

int vrb_eq_attach_domain(struct vrb_eq *eq, struct vrb_domain *domain)
{
	if (ofi_epoll_add(eq->epollfd, domain->verbs->async_fd,
			  OFI_EPOLL_IN, domain))
		return -errno;

	domain->eq = eq;
	ofi_atomic_inc32(&eq->ref);
	return 0;
}

int vrb_eq_detach_domain(struct vrb_domain *domain)
{
	if (ofi_epoll_del(domain->eq->epollfd, domain->verbs->async_fd))
		return -errno;

	ofi_atomic_dec32(&domain->eq->ref);
	domain->eq = NULL;
	return 0;
}

/* SPDX-License-Identifier: BSD-2-Clause OR GPL-2.0-only */
/* SPDX-FileCopyrightText: Copyright Amazon.com, Inc. or its affiliates. All rights reserved. */

#include "efa_unit_tests.h"
#include "efa_cq.h"
#include "efa_rdm_pke_utils.h"
#include "efa_rdm_pke_nonreq.h"
#include "efa_rdm_pke_req.h"

void efa_unit_test_construct_msg(struct fi_msg *msg, struct iovec *iov,
				 size_t iov_count, fi_addr_t addr,
				 void *context, uint64_t data,
				 void **desc)
{
	msg->msg_iov = iov;
	msg->iov_count = iov_count;
	msg->addr = addr;
	msg->context = context;
	msg->data = data;
	msg->desc = desc;
}

void efa_unit_test_construct_tmsg(struct fi_msg_tagged *tmsg, struct iovec *iov,
				  size_t iov_count, fi_addr_t addr,
				  void *context, uint64_t data,
				  void **desc, uint64_t tag,
				  uint64_t ignore)
{
	tmsg->msg_iov = iov;
	tmsg->iov_count = iov_count;
	tmsg->addr = addr;
	tmsg->context = context;
	tmsg->data = data;
	tmsg->desc = desc;
	tmsg->tag = tag;
	tmsg->ignore = ignore;
}

void efa_unit_test_construct_msg_rma(struct fi_msg_rma *msg, struct iovec *iov,
				     void **desc, size_t iov_count,
				     fi_addr_t addr, struct fi_rma_iov *rma_iov,
				     size_t rma_iov_count, void *context,
				     uint64_t data)
{
	msg->msg_iov = iov;
	msg->desc = desc;
	msg->iov_count = iov_count;
	msg->addr = addr;
	msg->rma_iov = rma_iov;
	msg->rma_iov_count = rma_iov_count;
	msg->context = context;
	msg->data = data;
}

struct fi_info *efa_unit_test_alloc_hints(enum fi_ep_type ep_type, char *fabric_name)
{
	struct fi_info *hints;

	hints = fi_allocinfo();
	if (!hints)
		return NULL;

	if (fabric_name)
		hints->fabric_attr->name = strdup(fabric_name);
	hints->ep_attr->type = ep_type;

	/* Use a minimal caps that efa / efa-direct should always support */
	hints->domain_attr->mr_mode = MR_MODE_BITS;

	/* EFA direct and dgram paths require FI_CONTEXT2 */
	if (!fabric_name || !strcasecmp(fabric_name, EFA_DIRECT_FABRIC_NAME))
		hints->mode |= FI_CONTEXT2;

	if (ep_type == FI_EP_DGRAM) {
		hints->mode |= FI_MSG_PREFIX | FI_CONTEXT2;
	}

	return hints;
}

struct fi_info *efa_unit_test_alloc_hints_hmem(enum fi_ep_type ep_type, char *fabric_name)
{
	struct fi_info *hints;

	hints = efa_unit_test_alloc_hints(ep_type, fabric_name);

	hints->caps |= FI_HMEM;
	hints->domain_attr->mr_mode |= FI_MR_HMEM;

	return hints;
}

void efa_unit_test_resource_construct_with_hints(struct efa_resource *resource,
						 enum fi_ep_type ep_type,
						 uint32_t fi_version, struct fi_info *hints,
						 bool enable_ep, bool open_cq)
{
	int ret = 0;
	struct fi_av_attr av_attr = {0};
	struct fi_cq_attr cq_attr = {
		.format = FI_CQ_FORMAT_DATA
	};
	struct fi_eq_attr eq_attr = {0};

	ret = fi_getinfo(fi_version, NULL, NULL, 0ULL, hints, &resource->info);
	if (ret)
		goto err;

	ret = fi_fabric(resource->info->fabric_attr, &resource->fabric, NULL);
	if (ret)
		goto err;

	ret = fi_domain(resource->fabric, resource->info, &resource->domain, NULL);
	if (ret)
		goto err;

	ret = fi_endpoint(resource->domain, resource->info, &resource->ep, NULL);
	if (ret)
		goto err;

	ret = fi_eq_open(resource->fabric, &eq_attr, &resource->eq, NULL);
	if (ret)
		goto err;

	fi_ep_bind(resource->ep, &resource->eq->fid, 0);

	ret = fi_av_open(resource->domain, &av_attr, &resource->av, NULL);
	if (ret)
		goto err;

	fi_ep_bind(resource->ep, &resource->av->fid, 0);

	if (open_cq) {
		ret = fi_cq_open(resource->domain, &cq_attr, &resource->cq, NULL);
		if (ret)
			goto err;

		fi_ep_bind(resource->ep, &resource->cq->fid, FI_SEND | FI_RECV);
	}

	if (enable_ep) {
		ret = fi_enable(resource->ep);
		if (ret)
			goto err;
	}

	return;

err:
	efa_unit_test_resource_destruct(resource);

	/* Fail test early if the resource struct fails to initialize */
	assert_int_equal(ret, 0);
}

void efa_unit_test_resource_construct(struct efa_resource *resource, enum fi_ep_type ep_type, char *fabric_name)
{

	resource->hints = efa_unit_test_alloc_hints(ep_type, fabric_name);
	if (!resource->hints)
		goto err;
	if (!strcmp(EFA_DIRECT_FABRIC_NAME, fabric_name))
		efa_unit_test_resource_construct_with_hints(resource, ep_type, FI_VERSION(2, 0),
							    resource->hints, true, true);
	else
		efa_unit_test_resource_construct_with_hints(resource, ep_type, FI_VERSION(1, 14),
							    resource->hints, true, true);
	return;

err:
	efa_unit_test_resource_destruct(resource);

	/* Fail test early if the resource struct fails to initialize */
	assert_int_equal(1, 0);
}

void efa_unit_test_resource_construct_ep_not_enabled(struct efa_resource *resource,
				      enum fi_ep_type ep_type, char *fabric_name)
{
	resource->hints = efa_unit_test_alloc_hints(ep_type, fabric_name);
	if (!resource->hints)
		goto err;

	if (!strcmp(EFA_DIRECT_FABRIC_NAME, fabric_name))
		efa_unit_test_resource_construct_with_hints(resource, ep_type, FI_VERSION(2, 0),
							    resource->hints, false, true);
	else
		efa_unit_test_resource_construct_with_hints(resource, ep_type, FI_VERSION(1, 14),
							    resource->hints, false, true);
	return;

err:
	efa_unit_test_resource_destruct(resource);

	/* Fail test early if the resource struct fails to initialize */
	fail();
}

void efa_unit_test_resource_construct_no_cq_and_ep_not_enabled(struct efa_resource *resource,
				      enum fi_ep_type ep_type, char *fabric_name)
{
	resource->hints = efa_unit_test_alloc_hints(ep_type, fabric_name);
	if (!resource->hints)
		goto err;

	if (!strcmp(EFA_DIRECT_FABRIC_NAME, fabric_name))
		efa_unit_test_resource_construct_with_hints(resource, ep_type, FI_VERSION(2, 0),
							    resource->hints, false, false);
	else
		efa_unit_test_resource_construct_with_hints(resource, ep_type, FI_VERSION(1, 14),
							    resource->hints, false, false);
	return;

err:
	efa_unit_test_resource_destruct(resource);

	/* Fail test early if the resource struct fails to initialize */
	fail();
}

/**
 * @brief Construct RDM ep type resources with shm disabled
 */
void efa_unit_test_resource_construct_rdm_shm_disabled(struct efa_resource *resource)
{
	int ret;
	bool shm_permitted = false;

	resource->hints = efa_unit_test_alloc_hints(FI_EP_RDM, EFA_FABRIC_NAME);
	if (!resource->hints)
		goto err;

	efa_unit_test_resource_construct_with_hints(resource, FI_EP_RDM, FI_VERSION(1, 14),
						    resource->hints, false, true);

	ret = fi_setopt(&resource->ep->fid, FI_OPT_ENDPOINT,
			FI_OPT_SHARED_MEMORY_PERMITTED, &shm_permitted,
			sizeof(shm_permitted));
	if (ret)
		goto err;

	ret = fi_enable(resource->ep);
	if (ret)
		goto err;

	return;
err:
	efa_unit_test_resource_destruct(resource);

	/* Fail test early if the resource struct fails to initialize */
	fail();
}

/**
 * @brief Clean up test resources.
 * Note: Resources should be destroyed in order.
 * @param[in] resource	struct efa_resource to clean up.
 */
void efa_unit_test_resource_destruct(struct efa_resource *resource)
{
	if (resource->ep) {
		assert_int_equal(fi_close(&resource->ep->fid), 0);
	}

	if (resource->eq) {
		assert_int_equal(fi_close(&resource->eq->fid), 0);
	}

	if (resource->cq) {
		assert_int_equal(fi_close(&resource->cq->fid), 0);
	}

	if (resource->av) {
		assert_int_equal(fi_close(&resource->av->fid), 0);
	}

	if (resource->domain) {
		assert_int_equal(fi_close(&resource->domain->fid), 0);
	}

	if (resource->fabric) {
		assert_int_equal(fi_close(&resource->fabric->fid), 0);
	}

	if (resource->info) {
		fi_freeinfo(resource->info);
	}

	if (resource->hints) {
		fi_freeinfo(resource->hints);
	}
}

void efa_unit_test_buff_construct(struct efa_unit_test_buff *buff, struct efa_resource *resource, size_t buff_size)
{
	int err;

	buff->buff = calloc(buff_size, sizeof(uint8_t));
	assert_non_null(buff->buff);

	buff->size = buff_size;
	err = fi_mr_reg(resource->domain, buff->buff, buff_size, FI_SEND | FI_RECV,
			0 /*offset*/, 0 /*requested_key*/, 0 /*flags*/, &buff->mr, NULL);
	assert_int_equal(err, 0);
}

void efa_unit_test_buff_destruct(struct efa_unit_test_buff *buff)
{
	int err;

	assert_non_null(buff->mr);
	err = fi_close(&buff->mr->fid);
	assert_int_equal(err, 0);

	free(buff->buff);
}

/**
 * @brief Construct EFA_RDM_EAGER_MSGRTM_PKT
 *
 * @param[in] pkt_entry Packet entry. Must be non-NULL.
 * @param[in] attr Packet attributes.
 */
void efa_unit_test_eager_msgrtm_pkt_construct(struct efa_rdm_pke *pkt_entry, struct efa_unit_test_eager_rtm_pkt_attr *attr)
{
	struct efa_rdm_eager_msgrtm_hdr base_hdr = {0};
	struct efa_rdm_req_opt_connid_hdr opt_connid_hdr = {0};
	uint32_t *connid = NULL;

	base_hdr.hdr.type = EFA_RDM_EAGER_MSGRTM_PKT;
	base_hdr.hdr.flags |= EFA_RDM_PKT_CONNID_HDR | EFA_RDM_REQ_MSG;
	base_hdr.hdr.msg_id = attr->msg_id;
	memcpy(pkt_entry->wiredata, &base_hdr, sizeof(struct efa_rdm_eager_msgrtm_hdr));
	assert_int_equal(efa_rdm_pke_get_base_hdr(pkt_entry)->type, EFA_RDM_EAGER_MSGRTM_PKT);
	assert_int_equal(efa_rdm_pke_get_req_base_hdr_size(pkt_entry), sizeof(struct efa_rdm_eager_msgrtm_hdr));
	opt_connid_hdr.connid = attr->connid;
	memcpy(pkt_entry->wiredata + sizeof(struct efa_rdm_eager_msgrtm_hdr), &opt_connid_hdr, sizeof(struct efa_rdm_req_opt_connid_hdr));
	connid = efa_rdm_pke_connid_ptr(pkt_entry);
	assert_int_equal(*connid, attr->connid);
	pkt_entry->pkt_size = sizeof(base_hdr) + sizeof(opt_connid_hdr);
}

#define APPEND_OPT_HANDSHAKE_FIELD(field, opt_flag)			\
        if (attr->field) {						\
                struct efa_rdm_handshake_opt_##field##_hdr *_hdr =	\
			(struct efa_rdm_handshake_opt_##field##_hdr *)	\
			(pkt_entry->wiredata + pkt_entry->pkt_size);	\
                _hdr->field = attr->field;				\
                handshake_hdr->flags |= opt_flag;			\
                pkt_entry->pkt_size += sizeof *_hdr;			\
        }

/**
 * @brief Construct EFA_RDM_HANDSHAKE_PKT
 *
 * This will append any optional handshake packet fields (see EFA RDM protocol
 * spec) iff they are non-zero in attr
 *
 * @param[in,out]	pkt_entry	Packet entry. Must be non-NULL.
 * @param[in]		attr		Packet attributes.
 */
void efa_unit_test_handshake_pkt_construct(struct efa_rdm_pke *pkt_entry, struct efa_unit_test_handshake_pkt_attr *attr)
{

	int nex = (EFA_RDM_NUM_EXTRA_FEATURE_OR_REQUEST - 1) / 64 + 1;
	struct efa_rdm_handshake_hdr *handshake_hdr = (struct efa_rdm_handshake_hdr *)pkt_entry->wiredata;

	handshake_hdr->type = EFA_RDM_HANDSHAKE_PKT;
	handshake_hdr->version = EFA_RDM_PROTOCOL_VERSION;
	handshake_hdr->nextra_p3 = nex + 3;
	handshake_hdr->flags = 0;

	pkt_entry->pkt_size = sizeof(struct efa_rdm_handshake_hdr) + nex * sizeof(uint64_t);

	APPEND_OPT_HANDSHAKE_FIELD(connid,		EFA_RDM_PKT_CONNID_HDR);
        APPEND_OPT_HANDSHAKE_FIELD(host_id,		EFA_RDM_HANDSHAKE_HOST_ID_HDR);
        APPEND_OPT_HANDSHAKE_FIELD(device_version,	EFA_RDM_HANDSHAKE_DEVICE_VERSION_HDR);
}


struct efa_rdm_ope *efa_unit_test_alloc_txe(struct efa_resource *resource, uint32_t op)
{
	fi_addr_t peer_addr = 0;
	struct efa_ep_addr raw_addr = {0};
	size_t raw_addr_len = sizeof(raw_addr);
	struct efa_rdm_peer *peer;
	struct fi_msg msg = {0};
	struct efa_rdm_ep *efa_rdm_ep;

	efa_rdm_ep = container_of(resource->ep, struct efa_rdm_ep, base_ep.util_ep.ep_fid);

	/* Create and register a fake peer */
	assert_int_equal(fi_getname(&resource->ep->fid, &raw_addr, &raw_addr_len), 0);
	raw_addr.qpn = 0;
	raw_addr.qkey = 0x1234;

	assert_int_equal(fi_av_insert(resource->av, &raw_addr, 1, &peer_addr, 0, NULL), 1);

	peer = efa_rdm_ep_get_peer(efa_rdm_ep, peer_addr);

	return efa_rdm_ep_alloc_txe(efa_rdm_ep, peer, &msg, op, 0, 0);
}

struct efa_rdm_ope *efa_unit_test_alloc_rxe(struct efa_resource *resource, uint32_t op)
{
	fi_addr_t peer_addr = 0;
	struct efa_ep_addr raw_addr = {0};
	size_t raw_addr_len = sizeof(raw_addr);
	struct efa_rdm_ep *efa_rdm_ep;

	efa_rdm_ep = container_of(resource->ep, struct efa_rdm_ep, base_ep.util_ep.ep_fid);

	/* Create and register a fake peer */
	assert_int_equal(fi_getname(&resource->ep->fid, &raw_addr, &raw_addr_len), 0);
	raw_addr.qpn = 0;
	raw_addr.qkey = 0x1234;

	assert_int_equal(fi_av_insert(resource->av, &raw_addr, 1, &peer_addr, 0, NULL), 1);

	struct efa_rdm_peer *peer = efa_rdm_ep_get_peer(efa_rdm_ep, 0);

	/* TODO - peer struct might need more info */
	return efa_rdm_ep_alloc_rxe(efa_rdm_ep, peer, op);
}

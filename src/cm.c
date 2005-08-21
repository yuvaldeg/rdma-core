/*
 * Copyright (c) 2005 Topspin Communications.  All rights reserved.
 * Copyright (c) 2005 Intel Corporation.  All rights reserved.
 *
 * This software is available to you under a choice of one of two
 * licenses.  You may choose to be licensed under the terms of the GNU
 * General Public License (GPL) Version 2, available from the file
 * COPYING in the main directory of this source tree, or the
 * OpenIB.org BSD license below:
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
 *
 * $Id$
 */

#if HAVE_CONFIG_H
#  include <config.h>
#endif /* HAVE_CONFIG_H */

#include <stdlib.h>
#include <string.h>
#include <glob.h>
#include <stdio.h>
#include <fcntl.h>
#include <errno.h>
#include <stdint.h>
#include <poll.h>
#include <unistd.h>
#include <pthread.h>

#include <infiniband/cm.h>
#include <infiniband/cm_abi.h>

#define IB_UCM_DEV_PATH "/dev/infiniband/ucm"
#define PFX "libucm: "

#define CM_CREATE_MSG_CMD_RESP(msg, cmd, resp, type, size) \
do {                                        \
	struct cm_abi_cmd_hdr *hdr;         \
                                            \
	size = sizeof(*hdr) + sizeof(*cmd); \
	msg = alloca(size);                 \
	if (!msg)                           \
		return -ENOMEM;             \
	hdr = msg;                          \
	cmd = msg + sizeof(*hdr);           \
	hdr->cmd = type;                    \
	hdr->in  = sizeof(*cmd);            \
	hdr->out = sizeof(*resp);           \
	memset(cmd, 0, sizeof(*cmd));       \
	resp = alloca(sizeof(*resp));       \
	if (!resp)                          \
		return -ENOMEM;             \
	cmd->response = (uintptr_t)resp;\
} while (0)

#define CM_CREATE_MSG_CMD(msg, cmd, type, size) \
do {                                        \
	struct cm_abi_cmd_hdr *hdr;         \
                                            \
	size = sizeof(*hdr) + sizeof(*cmd); \
	msg = alloca(size);                 \
	if (!msg)                           \
		return -ENOMEM;             \
	hdr = msg;                          \
	cmd = msg + sizeof(*hdr);           \
	hdr->cmd = type;                    \
	hdr->in  = sizeof(*cmd);            \
	hdr->out = 0;                       \
	memset(cmd, 0, sizeof(*cmd));       \
} while (0)

struct cm_id_private {
	struct ib_cm_id id;
	int events_completed;
	pthread_cond_t cond;
	pthread_mutex_t mut;
};

static int fd;

#define container_of(ptr, type, field) \
	((type *) ((void *)ptr - offsetof(type, field)))

static void __attribute__((constructor)) ib_cm_init(void)
{
	fd = open(IB_UCM_DEV_PATH, O_RDWR);
        if (fd < 0)
		fprintf(stderr, PFX
			"Error <%d:%d> couldn't open IB cm device <%s>\n",
			fd, errno, IB_UCM_DEV_PATH);

}

static void cm_param_path_get(struct cm_abi_path_rec *abi,
			      struct ib_sa_path_rec *sa)
{
	memcpy(abi->dgid, sa->dgid.raw, sizeof(union ibv_gid));
	memcpy(abi->sgid, sa->sgid.raw, sizeof(union ibv_gid));

	abi->dlid = sa->dlid;
	abi->slid = sa->slid;

	abi->raw_traffic  = sa->raw_traffic;
	abi->flow_label   = sa->flow_label;
	abi->reversible   = sa->reversible;
	abi->mtu          = sa->mtu;
	abi->pkey         = sa->pkey;

	abi->hop_limit                 = sa->hop_limit;
	abi->traffic_class             = sa->traffic_class;
	abi->numb_path                 = sa->numb_path;
	abi->sl                        = sa->sl;
	abi->mtu_selector              = sa->mtu_selector;
	abi->rate_selector             = sa->rate_selector;
	abi->rate                      = sa->rate;
	abi->packet_life_time_selector = sa->packet_life_time_selector;
	abi->packet_life_time          = sa->packet_life_time;
	abi->preference                = sa->preference;
}

static void ib_cm_free_id(struct cm_id_private *cm_id_priv)
{
	pthread_cond_destroy(&cm_id_priv->cond);
	pthread_mutex_destroy(&cm_id_priv->mut);
	free(cm_id_priv);
}

static struct cm_id_private *ib_cm_alloc_id(void *context)
{
	struct cm_id_private *cm_id_priv;

	cm_id_priv = malloc(sizeof *cm_id_priv);
	if (!cm_id_priv)
		return NULL;

	memset(cm_id_priv, 0, sizeof *cm_id_priv);
	cm_id_priv->id.context = context;
	pthread_mutex_init(&cm_id_priv->mut, NULL);
	if (pthread_cond_init(&cm_id_priv->cond, NULL))
		goto err;

	return cm_id_priv;

err:	ib_cm_free_id(cm_id_priv);
	return NULL;
}

int ib_cm_create_id(struct ib_cm_id **cm_id, void *context)
{
	struct cm_abi_create_id_resp *resp;
	struct cm_abi_create_id *cmd;
	struct cm_id_private *cm_id_priv;
	void *msg;
	int result;
	int size;

	cm_id_priv = ib_cm_alloc_id(context);
	if (!cm_id_priv)
		return -ENOMEM;

	CM_CREATE_MSG_CMD_RESP(msg, cmd, resp, IB_USER_CM_CMD_CREATE_ID, size);
	cmd->uid = (uintptr_t) cm_id_priv;

	result = write(fd, msg, size);
	if (result != size)
		goto err;

	cm_id_priv->id.handle = resp->id;
	*cm_id = &cm_id_priv->id;
	return 0;

err:	ib_cm_free_id(cm_id_priv);
	return result;
}

int ib_cm_destroy_id(struct ib_cm_id *cm_id)
{
	struct cm_abi_destroy_id_resp *resp;
	struct cm_abi_destroy_id *cmd;
	struct cm_id_private *cm_id_priv;
	void *msg;
	int result;
	int size;
	
	CM_CREATE_MSG_CMD_RESP(msg, cmd, resp, IB_USER_CM_CMD_DESTROY_ID, size);
	cmd->id = cm_id->handle;

	result = write(fd, msg, size);
	if (result != size)
		return (result > 0) ? -ENODATA : result;

	cm_id_priv = container_of(cm_id, struct cm_id_private, id);

	pthread_mutex_lock(&cm_id_priv->mut);
	while (cm_id_priv->events_completed < resp->events_reported)
		pthread_cond_wait(&cm_id_priv->cond, &cm_id_priv->mut);
	pthread_mutex_unlock(&cm_id_priv->mut);

	ib_cm_free_id(cm_id_priv);
	return 0;
}

int ib_cm_attr_id(struct ib_cm_id *cm_id, struct ib_cm_attr_param *param)
{
	struct cm_abi_attr_id_resp *resp;
	struct cm_abi_attr_id *cmd;
	void *msg;
	int result;
	int size;

	if (!param)
		return -EINVAL;

	CM_CREATE_MSG_CMD_RESP(msg, cmd, resp, IB_USER_CM_CMD_ATTR_ID, size);
	cmd->id = cm_id->handle;

	result = write(fd, msg, size);
	if (result != size)
		return (result > 0) ? -ENODATA : result;

	param->service_id   = resp->service_id;
	param->service_mask = resp->service_mask;
	param->local_id     = resp->local_id;
	param->remote_id    = resp->remote_id;
	return 0;
}

static void ib_cm_copy_ah_attr(struct ibv_ah_attr *dest_attr,
			       struct cm_abi_ah_attr *src_attr)
{
	memcpy(dest_attr->grh.dgid.raw, src_attr->grh_dgid,
	       sizeof dest_attr->grh.dgid);
	dest_attr->grh.flow_label = src_attr->grh_flow_label;
	dest_attr->grh.sgid_index = src_attr->grh_sgid_index;
	dest_attr->grh.hop_limit = src_attr->grh_hop_limit;
	dest_attr->grh.traffic_class = src_attr->grh_traffic_class;

	dest_attr->dlid = src_attr->dlid;
	dest_attr->sl = src_attr->sl;
	dest_attr->src_path_bits = src_attr->src_path_bits;
	dest_attr->static_rate = src_attr->static_rate;
	dest_attr->is_global = src_attr->is_global;
	dest_attr->port_num = src_attr->port_num;
}

static void ib_cm_copy_qp_attr(struct ibv_qp_attr *dest_attr,
			       struct cm_abi_init_qp_attr_resp *src_attr)
{
	dest_attr->cur_qp_state = src_attr->cur_qp_state;
	dest_attr->path_mtu = src_attr->path_mtu;
	dest_attr->path_mig_state = src_attr->path_mig_state;
	dest_attr->qkey = src_attr->qkey;
	dest_attr->rq_psn = src_attr->rq_psn;
	dest_attr->sq_psn = src_attr->sq_psn;
	dest_attr->dest_qp_num = src_attr->dest_qp_num;
	dest_attr->qp_access_flags = src_attr->qp_access_flags;

	dest_attr->cap.max_send_wr = src_attr->max_send_wr;
	dest_attr->cap.max_recv_wr = src_attr->max_recv_wr;
	dest_attr->cap.max_send_sge = src_attr->max_send_sge;
	dest_attr->cap.max_recv_sge = src_attr->max_recv_sge;
	dest_attr->cap.max_inline_data = src_attr->max_inline_data;

	ib_cm_copy_ah_attr(&dest_attr->ah_attr, &src_attr->ah_attr);
	ib_cm_copy_ah_attr(&dest_attr->alt_ah_attr, &src_attr->alt_ah_attr);

	dest_attr->pkey_index = src_attr->pkey_index;
	dest_attr->alt_pkey_index = src_attr->alt_pkey_index;
	dest_attr->en_sqd_async_notify = src_attr->en_sqd_async_notify;
	dest_attr->sq_draining = src_attr->sq_draining;
	dest_attr->max_rd_atomic = src_attr->max_rd_atomic;
	dest_attr->max_dest_rd_atomic = src_attr->max_dest_rd_atomic;
	dest_attr->min_rnr_timer = src_attr->min_rnr_timer;
	dest_attr->port_num = src_attr->port_num;
	dest_attr->timeout = src_attr->timeout;
	dest_attr->retry_cnt = src_attr->retry_cnt;
	dest_attr->rnr_retry = src_attr->rnr_retry;
	dest_attr->alt_port_num = src_attr->alt_port_num;
	dest_attr->alt_timeout = src_attr->alt_timeout;
}

int ib_cm_init_qp_attr(struct ib_cm_id *cm_id,
		       struct ibv_qp_attr *qp_attr,
		       int *qp_attr_mask)
{
	struct cm_abi_init_qp_attr_resp *resp;
	struct cm_abi_init_qp_attr *cmd;
	void *msg;
	int result;
	int size;

	if (!qp_attr || !qp_attr_mask)
		return -EINVAL;

	CM_CREATE_MSG_CMD_RESP(msg, cmd, resp, IB_USER_CM_CMD_INIT_QP_ATTR, size);
	cmd->id = cm_id->handle;
	cmd->qp_state = qp_attr->qp_state;

	result = write(fd, msg, size);
	if (result != size)
		return (result > 0) ? -ENODATA : result;

	*qp_attr_mask = resp->qp_attr_mask;
	ib_cm_copy_qp_attr(qp_attr, resp);

	return 0;
}

int ib_cm_listen(struct ib_cm_id *cm_id,
		 uint64_t service_id,
		 uint64_t service_mask)
{
	struct cm_abi_listen *cmd;
	void *msg;
	int result;
	int size;
	
	CM_CREATE_MSG_CMD(msg, cmd, IB_USER_CM_CMD_LISTEN, size);
	cmd->id           = cm_id->handle;
	cmd->service_id   = service_id;
	cmd->service_mask = service_mask;

	result = write(fd, msg, size);
	if (result != size)
		return (result > 0) ? -ENODATA : result;

	return 0;
}

int ib_cm_send_req(struct ib_cm_id *cm_id, struct ib_cm_req_param *param)
{
	struct cm_abi_path_rec *p_path;
	struct cm_abi_path_rec *a_path;
	struct cm_abi_req *cmd;
	void *msg;
	int result;
	int size;

	if (!param)
		return -EINVAL;

	CM_CREATE_MSG_CMD(msg, cmd, IB_USER_CM_CMD_SEND_REQ, size);
	cmd->id				= cm_id->handle;
	cmd->qpn			= param->qp_num;
	cmd->qp_type			= param->qp_type;
	cmd->psn			= param->starting_psn;
        cmd->sid			= param->service_id;
        cmd->peer_to_peer               = param->peer_to_peer;
        cmd->responder_resources        = param->responder_resources;
        cmd->initiator_depth            = param->initiator_depth;
        cmd->remote_cm_response_timeout = param->remote_cm_response_timeout;
        cmd->flow_control               = param->flow_control;
        cmd->local_cm_response_timeout  = param->local_cm_response_timeout;
        cmd->retry_count                = param->retry_count;
        cmd->rnr_retry_count            = param->rnr_retry_count;
        cmd->max_cm_retries             = param->max_cm_retries;
        cmd->srq                        = param->srq;

	if (param->primary_path) {
		p_path = alloca(sizeof(*p_path));
		if (!p_path)
			return -ENOMEM;

		cm_param_path_get(p_path, param->primary_path);
		cmd->primary_path = (uintptr_t) p_path;
	}
		
	if (param->alternate_path) {
		a_path = alloca(sizeof(*a_path));
		if (!a_path)
			return -ENOMEM;

		cm_param_path_get(a_path, param->alternate_path);
		cmd->alternate_path = (uintptr_t) a_path;
	}

	if (param->private_data && param->private_data_len) {
		cmd->data = (uintptr_t) param->private_data;
		cmd->len  = param->private_data_len;
	}

	result = write(fd, msg, size);
	if (result != size)
		return (result > 0) ? -ENODATA : result;

	return 0;
}

int ib_cm_send_rep(struct ib_cm_id *cm_id, struct ib_cm_rep_param *param)
{
	struct cm_abi_rep *cmd;
	void *msg;
	int result;
	int size;

	if (!param)
		return -EINVAL;

	CM_CREATE_MSG_CMD(msg, cmd, IB_USER_CM_CMD_SEND_REP, size);
	cmd->uid = (uintptr_t) container_of(cm_id, struct cm_id_private, id);
	cmd->id			 = cm_id->handle;
	cmd->qpn		 = param->qp_num;
	cmd->psn		 = param->starting_psn;
        cmd->responder_resources = param->responder_resources;
        cmd->initiator_depth     = param->initiator_depth;
	cmd->target_ack_delay    = param->target_ack_delay;
	cmd->failover_accepted   = param->failover_accepted;
        cmd->flow_control        = param->flow_control;
        cmd->rnr_retry_count     = param->rnr_retry_count;
        cmd->srq                 = param->srq;

	if (param->private_data && param->private_data_len) {
		cmd->data = (uintptr_t) param->private_data;
		cmd->len  = param->private_data_len;
	}

	result = write(fd, msg, size);
	if (result != size)
		return (result > 0) ? -ENODATA : result;

	return 0;
}

static inline int cm_send_private_data(struct ib_cm_id *cm_id,
				       uint32_t type,
				       void *private_data,
				       uint8_t private_data_len)
{
	struct cm_abi_private_data *cmd;
	void *msg;
	int result;
	int size;

	CM_CREATE_MSG_CMD(msg, cmd, type, size);
	cmd->id = cm_id->handle;

	if (private_data && private_data_len) {
		cmd->data = (uintptr_t) private_data;
		cmd->len  = private_data_len;
	}

	result = write(fd, msg, size);
	if (result != size)
		return (result > 0) ? -ENODATA : result;

	return 0;
}

int ib_cm_send_rtu(struct ib_cm_id *cm_id,
		   void *private_data,
		   uint8_t private_data_len)
{
	return cm_send_private_data(cm_id, IB_USER_CM_CMD_SEND_RTU,
				    private_data, private_data_len);
}

int ib_cm_send_dreq(struct ib_cm_id *cm_id,
		    void *private_data,
		    uint8_t private_data_len)
{
	return cm_send_private_data(cm_id, IB_USER_CM_CMD_SEND_DREQ,
				    private_data, private_data_len);
}

int ib_cm_send_drep(struct ib_cm_id *cm_id,
		    void *private_data,
		    uint8_t private_data_len)
{
	return cm_send_private_data(cm_id, IB_USER_CM_CMD_SEND_DREP,
				    private_data, private_data_len);
}

int ib_cm_establish(struct ib_cm_id *cm_id)
{
	struct cm_abi_establish *cmd;
	void *msg;
	int result;
	int size;
	
	CM_CREATE_MSG_CMD(msg, cmd, IB_USER_CM_CMD_ESTABLISH, size);
	cmd->id = cm_id->handle;

	result = write(fd, msg, size);
	if (result != size)
		return (result > 0) ? -ENODATA : result;

	return 0;
}

static inline int cm_send_status(struct ib_cm_id *cm_id,
				 uint32_t type,
				 int status,
				 void *info,
				 uint8_t info_length,
				 void *private_data,
				 uint8_t private_data_len)
{
	struct cm_abi_info *cmd;
	void *msg;
	int result;
	int size;

	CM_CREATE_MSG_CMD(msg, cmd, type, size);
	cmd->id     = cm_id->handle;
	cmd->status = status;

	if (private_data && private_data_len) {
		cmd->data     = (uintptr_t) private_data;
		cmd->data_len = private_data_len;
	}

	if (info && info_length) {
		cmd->info     = (uintptr_t) info;
		cmd->info_len = info_length;
	}

	result = write(fd, msg, size);
	if (result != size)
		return (result > 0) ? -ENODATA : result;

	return 0;
}

int ib_cm_send_rej(struct ib_cm_id *cm_id,
		   enum ib_cm_rej_reason reason,
		   void *ari,
		   uint8_t ari_length,
		   void *private_data,
		   uint8_t private_data_len)
{
	return cm_send_status(cm_id, IB_USER_CM_CMD_SEND_REJ, reason, 
			      ari, ari_length,
			      private_data, private_data_len);
}

int ib_cm_send_apr(struct ib_cm_id *cm_id,
		   enum ib_cm_apr_status status,
		   void *info,
		   uint8_t info_length,
		   void *private_data,
		   uint8_t private_data_len)
{
	return cm_send_status(cm_id, IB_USER_CM_CMD_SEND_APR, status, 
			      info, info_length,
			      private_data, private_data_len);
}

int ib_cm_send_mra(struct ib_cm_id *cm_id,
		   uint8_t service_timeout,
		   void *private_data,
		   uint8_t private_data_len)
{
	struct cm_abi_mra *cmd;
	void *msg;
	int result;
	int size;

	CM_CREATE_MSG_CMD(msg, cmd, IB_USER_CM_CMD_SEND_MRA, size);
	cmd->id      = cm_id->handle;
	cmd->timeout = service_timeout;

	if (private_data && private_data_len) {
		cmd->data = (uintptr_t) private_data;
		cmd->len  = private_data_len;
	}

	result = write(fd, msg, size);
	if (result != size)
		return (result > 0) ? -ENODATA : result;

	return 0;
}

int ib_cm_send_lap(struct ib_cm_id *cm_id,
		   struct ib_sa_path_rec *alternate_path,
		   void *private_data,
		   uint8_t private_data_len)
{
	struct cm_abi_path_rec *abi_path;
	struct cm_abi_lap *cmd;
	void *msg;
	int result;
	int size;

	CM_CREATE_MSG_CMD(msg, cmd, IB_USER_CM_CMD_SEND_LAP, size);
	cmd->id = cm_id->handle;

	if (alternate_path) {
		abi_path = alloca(sizeof(*abi_path));
		if (!abi_path)
			return -ENOMEM;

		cm_param_path_get(abi_path, alternate_path);
		cmd->path = (uintptr_t) abi_path;
	}

	if (private_data && private_data_len) {
		cmd->data = (uintptr_t) private_data;
		cmd->len  = private_data_len;
	}

	result = write(fd, msg, size);
	if (result != size)
		return (result > 0) ? -ENODATA : result;

	return 0;
}

int ib_cm_send_sidr_req(struct ib_cm_id *cm_id,
			struct ib_cm_sidr_req_param *param)
{
	struct cm_abi_path_rec *abi_path;
	struct cm_abi_sidr_req *cmd;
	void *msg;
	int result;
	int size;

	if (!param)
		return -EINVAL;

	CM_CREATE_MSG_CMD(msg, cmd, IB_USER_CM_CMD_SEND_SIDR_REQ, size);
	cmd->id             = cm_id->handle;
	cmd->sid            = param->service_id;
	cmd->timeout        = param->timeout_ms;
	cmd->pkey           = param->pkey;
	cmd->max_cm_retries = param->max_cm_retries;

	if (param->path) {
		abi_path = alloca(sizeof(*abi_path));
		if (!abi_path)
			return -ENOMEM;

		cm_param_path_get(abi_path, param->path);
		cmd->path = (uintptr_t) abi_path;
	}

	if (param->private_data && param->private_data_len) {
		cmd->data = (uintptr_t) param->private_data;
		cmd->len  = param->private_data_len;
	}

	result = write(fd, msg, size);
	if (result != size)
		return (result > 0) ? -ENODATA : result;

	return 0;
}

int ib_cm_send_sidr_rep(struct ib_cm_id *cm_id,
			struct ib_cm_sidr_rep_param *param)
{
	struct cm_abi_sidr_rep *cmd;
	void *msg;
	int result;
	int size;

	if (!param)
		return -EINVAL;

	CM_CREATE_MSG_CMD(msg, cmd, IB_USER_CM_CMD_SEND_SIDR_REP, size);
	cmd->id     = cm_id->handle;
	cmd->qpn    = param->qp_num;
	cmd->qkey   = param->qkey;
	cmd->status = param->status;

	if (param->private_data && param->private_data_len) {
		cmd->data     = (uintptr_t) param->private_data;
		cmd->data_len = param->private_data_len;
	}

	if (param->info && param->info_length) {
		cmd->info     = (uintptr_t) param->info;
		cmd->info_len = param->info_length;
	}

	result = write(fd, msg, size);
	if (result != size)
		return (result > 0) ? -ENODATA : result;

	return 0;
}

/*
 * event processing
 */
static void cm_event_path_get(struct ib_sa_path_rec  *upath,
			      struct cm_abi_path_rec *kpath)
{
	if (!kpath || !upath)
		return;

	memcpy(upath->dgid.raw, kpath->dgid, sizeof upath->dgid);
	memcpy(upath->sgid.raw, kpath->sgid, sizeof upath->sgid);
	
	upath->dlid             = kpath->dlid;
	upath->slid             = kpath->slid;
	upath->raw_traffic      = kpath->raw_traffic;
	upath->flow_label       = kpath->flow_label;
	upath->hop_limit        = kpath->hop_limit;
	upath->traffic_class    = kpath->traffic_class;
	upath->reversible       = kpath->reversible;
	upath->numb_path        = kpath->numb_path;
	upath->pkey             = kpath->pkey;
	upath->sl	        = kpath->sl;
	upath->mtu_selector     = kpath->mtu_selector;
	upath->mtu              = kpath->mtu;
	upath->rate_selector    = kpath->rate_selector;
	upath->rate             = kpath->rate;
	upath->packet_life_time = kpath->packet_life_time;
	upath->preference       = kpath->preference;

	upath->packet_life_time_selector = 
		kpath->packet_life_time_selector;
}

static void cm_event_req_get(struct ib_cm_req_event_param *ureq,
			     struct cm_abi_req_event_resp *kreq)
{
	ureq->remote_ca_guid             = kreq->remote_ca_guid;
	ureq->remote_qkey                = kreq->remote_qkey;
	ureq->remote_qpn                 = kreq->remote_qpn;
	ureq->qp_type                    = kreq->qp_type;
	ureq->starting_psn               = kreq->starting_psn;
	ureq->responder_resources        = kreq->responder_resources;
	ureq->initiator_depth            = kreq->initiator_depth;
	ureq->local_cm_response_timeout  = kreq->local_cm_response_timeout;
	ureq->flow_control               = kreq->flow_control;
	ureq->remote_cm_response_timeout = kreq->remote_cm_response_timeout;
	ureq->retry_count                = kreq->retry_count;
	ureq->rnr_retry_count            = kreq->rnr_retry_count;
	ureq->srq                        = kreq->srq;

	cm_event_path_get(ureq->primary_path, &kreq->primary_path);
	cm_event_path_get(ureq->alternate_path, &kreq->alternate_path);
}

static void cm_event_rep_get(struct ib_cm_rep_event_param *urep,
			     struct cm_abi_rep_event_resp *krep)
{
	urep->remote_ca_guid      = krep->remote_ca_guid;
	urep->remote_qkey         = krep->remote_qkey;
	urep->remote_qpn          = krep->remote_qpn;
	urep->starting_psn        = krep->starting_psn;
	urep->responder_resources = krep->responder_resources;
	urep->initiator_depth     = krep->initiator_depth;
	urep->target_ack_delay    = krep->target_ack_delay;
	urep->failover_accepted   = krep->failover_accepted;
	urep->flow_control        = krep->flow_control;
	urep->rnr_retry_count     = krep->rnr_retry_count;
	urep->srq                 = krep->srq;
}

static void cm_event_sidr_rep_get(struct ib_cm_sidr_rep_event_param *urep,
				  struct cm_abi_sidr_rep_event_resp *krep)
{
	urep->status = krep->status;
	urep->qkey   = krep->qkey;
	urep->qpn    = krep->qpn;
};

int ib_cm_event_get(struct ib_cm_event **event)
{
	struct cm_id_private *cm_id_priv;
	struct cm_abi_cmd_hdr *hdr;
	struct cm_abi_event_get *cmd;
	struct cm_abi_event_resp *resp;
	struct ib_cm_event *evt = NULL;
	struct ib_sa_path_rec *path_a = NULL;
	struct ib_sa_path_rec *path_b = NULL;
	void *data = NULL;
	void *info = NULL;
	void *msg;
	int result = 0;
	int size;
	
	if (!event)
		return -EINVAL;

	size = sizeof(*hdr) + sizeof(*cmd);
	msg = alloca(size);
	if (!msg)
		return -ENOMEM;
	
	hdr = msg;
	cmd = msg + sizeof(*hdr);

	hdr->cmd = IB_USER_CM_CMD_EVENT;
	hdr->in  = sizeof(*cmd);
	hdr->out = sizeof(*resp);

	resp = alloca(sizeof(*resp));
	if (!resp)
		return -ENOMEM;
	
	cmd->response = (uintptr_t) resp;
	cmd->data_len = (uint8_t)(~0U);
	cmd->info_len = (uint8_t)(~0U);

	data = malloc(cmd->data_len);
	if (!data) {
		result = -ENOMEM;
		goto done;
	}

	info = malloc(cmd->info_len);
	if (!info) {
		result = -ENOMEM;
		goto done;
	}

	cmd->data = (uintptr_t) data;
	cmd->info = (uintptr_t) info;

	result = write(fd, msg, size);
	if (result != size) {
		result = (result > 0) ? -ENODATA : result;
		goto done;
	}
	/*
	 * decode event.
	 */
	evt = malloc(sizeof(*evt));
	if (!evt) {
		result = -ENOMEM;
		goto done;
	}
	memset(evt, 0, sizeof(*evt));
	evt->cm_id = (void *) (uintptr_t) resp->uid;
	evt->event = resp->event;

	if (resp->present & CM_ABI_PRES_PRIMARY) {
		path_a = malloc(sizeof(*path_a));
		if (!path_a) {
			result = -ENOMEM;
			goto done;
		}
	}

	if (resp->present & CM_ABI_PRES_ALTERNATE) {
		path_b = malloc(sizeof(*path_b));
		if (!path_b) {
			result = -ENOMEM;
			goto done;
		}
	}

	switch (evt->event) {
	case IB_CM_REQ_RECEIVED:
		evt->param.req_rcvd.listen_id = evt->cm_id;
		cm_id_priv = ib_cm_alloc_id(evt->cm_id->context);
		if (!cm_id_priv) {
			result = -ENOMEM;
			goto done;
		}
		cm_id_priv->id.handle = resp->id;
		evt->cm_id = &cm_id_priv->id;
		evt->param.req_rcvd.primary_path   = path_a;
		evt->param.req_rcvd.alternate_path = path_b;
		path_a = NULL;
		path_b = NULL;
		cm_event_req_get(&evt->param.req_rcvd, &resp->u.req_resp);
		break;
	case IB_CM_REP_RECEIVED:
		cm_event_rep_get(&evt->param.rep_rcvd, &resp->u.rep_resp);
		break;
	case IB_CM_MRA_RECEIVED:
		evt->param.mra_rcvd.service_timeout = resp->u.mra_resp.timeout;
		break;
	case IB_CM_REJ_RECEIVED:
		evt->param.rej_rcvd.reason = resp->u.rej_resp.reason;
		evt->param.rej_rcvd.ari = info;
		info = NULL;
		break;
	case IB_CM_LAP_RECEIVED:
		evt->param.lap_rcvd.alternate_path = path_b;
		path_b = NULL;
		cm_event_path_get(evt->param.lap_rcvd.alternate_path,
				  &resp->u.lap_resp.path);
		break;
	case IB_CM_APR_RECEIVED:
		evt->param.apr_rcvd.ap_status = resp->u.apr_resp.status;
		evt->param.apr_rcvd.apr_info = info;
		info = NULL;
		break;
	case IB_CM_SIDR_REQ_RECEIVED:
		evt->param.sidr_req_rcvd.listen_id = evt->cm_id;
		cm_id_priv = ib_cm_alloc_id(evt->cm_id->context);
		if (!cm_id_priv) {
			result = -ENOMEM;
			goto done;
		}
		cm_id_priv->id.handle = resp->id;
		evt->cm_id = &cm_id_priv->id;
		evt->param.sidr_req_rcvd.pkey = resp->u.sidr_req_resp.pkey;
		break;
	case IB_CM_SIDR_REP_RECEIVED:
		cm_event_sidr_rep_get(&evt->param.sidr_rep_rcvd,
				      &resp->u.sidr_rep_resp);
		evt->param.sidr_rep_rcvd.info = info;
		info = NULL;
		break;
	default:
		evt->param.send_status = resp->u.send_status;
		break;
	}

	if (resp->present & CM_ABI_PRES_DATA) {
		evt->private_data = data;
		data = NULL;
	}

	*event = evt;
	evt    = NULL;
	result = 0;
done:
	if (data)
		free(data);
	if (info)
		free(info);
	if (path_a)
		free(path_a);
	if (path_b)
		free(path_b);
	if (evt)
		free(evt);

	return result;
}

int ib_cm_event_put(struct ib_cm_event *event)
{
	struct cm_id_private *cm_id_priv;

	if (!event)
		return -EINVAL;

	if (event->private_data)
		free(event->private_data);

	cm_id_priv = container_of(event->cm_id, struct cm_id_private, id);

	switch (event->event) {
	case IB_CM_REQ_RECEIVED:
		cm_id_priv = container_of(event->param.req_rcvd.listen_id,
					  struct cm_id_private, id);
		free(event->param.req_rcvd.primary_path);
		if (event->param.req_rcvd.alternate_path)
			free(event->param.req_rcvd.alternate_path);
		break;
	case IB_CM_REJ_RECEIVED:
		if (event->param.rej_rcvd.ari)
			free(event->param.rej_rcvd.ari);
		break;
	case IB_CM_LAP_RECEIVED:
		free(event->param.lap_rcvd.alternate_path);
		break;
	case IB_CM_APR_RECEIVED:
		if (event->param.apr_rcvd.apr_info)
			free(event->param.apr_rcvd.apr_info);
		break;
	case IB_CM_SIDR_REQ_RECEIVED:
		cm_id_priv = container_of(event->param.sidr_req_rcvd.listen_id,
					  struct cm_id_private, id);
		break;
	case IB_CM_SIDR_REP_RECEIVED:
		if (event->param.sidr_rep_rcvd.info)
			free(event->param.sidr_rep_rcvd.info);
	default:
		break;
	}

	pthread_mutex_lock(&cm_id_priv->mut);
	cm_id_priv->events_completed++;
	pthread_cond_signal(&cm_id_priv->cond);
	pthread_mutex_unlock(&cm_id_priv->mut);

	free(event);
	return 0;
}

int ib_cm_get_fd(void)
{
	return fd;
}

int ib_cm_event_get_timed(int timeout_ms, struct ib_cm_event **event)
{
	struct pollfd ufds;
	int result;

	if (!event)
		return -EINVAL;

	ufds.fd      = ib_cm_get_fd();
	ufds.events  = POLLIN;
	ufds.revents = 0;

	*event = NULL;

	result = poll(&ufds, 1, timeout_ms);
	if (!result)
		return -ETIMEDOUT;

	if (result < 0)
		return result;

	return ib_cm_event_get(event);
}

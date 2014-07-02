/*
 * Copyright (c) 2013 Mellanox Technologies®. All rights reserved.
 *
 * This software is available to you under a choice of one of two licenses.
 * You may choose to be licensed under the terms of the GNU General Public
 * License (GPL) Version 2, available from the file COPYING in the main
 * directory of this source tree, or the Mellanox Technologies® BSD license
 * below:
 *
 *      - Redistribution and use in source and binary forms, with or without
 *        modification, are permitted provided that the following conditions
 *        are met:
 *
 *      - Redistributions of source code must retain the above copyright
 *        notice, this list of conditions and the following disclaimer.
 *
 *      - Redistributions in binary form must reproduce the above
 *        copyright notice, this list of conditions and the following
 *        disclaimer in the documentation and/or other materials
 *        provided with the distribution.
 *
 *      - Neither the name of the Mellanox Technologies® nor the names of its
 *        contributors may be used to endorse or promote products derived from
 *        this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 */

#include "xio_common.h"
#include "xio_observer.h"
#include "xio_log.h"
#include "xio_task.h"
#include "xio_tcp_transport.h"

extern struct xio_tcp_options tcp_options;

static int xio_tcp_xmit(struct xio_tcp_transport *tcp_hndl);

/*---------------------------------------------------------------------------*/
/* xio_tcp_send                                                           */
/*---------------------------------------------------------------------------*/
static int xio_tcp_send_work(struct xio_tcp_transport *tcp_hndl,
			     struct xio_tcp_work_req *xio_send,
			     int block)
{
	int			i, retval = 0, tmp_bytes, sent_bytes = 0;

	while (xio_send->tot_iov_byte_len) {
		retval = sendmsg(tcp_hndl->sock_fd, &xio_send->msg, 0);
		if (retval < 0) {
			if (errno != EAGAIN) {
				xio_set_error(errno);
				ERROR_LOG("sendmsg failed. (errno=%d %s)\n",
					  errno, strerror(retval));
				/* ORK todo how to recover on remote side?*/
				return -1;
			} else if (!block) {
				xio_set_error(errno);
				/* ORK todo set epollout event
				 * to trigger send again */
				/* ORK todo polling on sendmsg few more times
				 * before returning*/
				return -1;
			}
		} else {
			sent_bytes += retval;
			xio_send->tot_iov_byte_len -= retval;

			if (xio_send->tot_iov_byte_len == 0)
				break;

			tmp_bytes = 0;
			for (i = 0; i < xio_send->msg.msg_iovlen; i++) {
				if (xio_send->msg.msg_iov[i].iov_len +
						tmp_bytes < retval) {
					tmp_bytes +=
					xio_send->msg.msg_iov[i].iov_len;
				} else {
					xio_send->msg.msg_iov[i].iov_len -=
							(retval - tmp_bytes);
					xio_send->msg.msg_iov[i].iov_base +=
							(retval - tmp_bytes);
					xio_send->msg.msg_iov =
						&xio_send->msg.msg_iov[i];
					xio_send->msg.msg_iovlen -= i;
					break;
				}
			}
		}
	}

	return sent_bytes;
}

/*---------------------------------------------------------------------------*/
/* xio_tcp_write_setup_msg						     */
/*---------------------------------------------------------------------------*/
static void xio_tcp_write_setup_msg(struct xio_tcp_transport *tcp_hndl,
				    struct xio_task *task,
				    struct xio_tcp_setup_msg *msg)
{
	struct xio_tcp_setup_msg	*tmp_msg;

	/* set the mbuf after tlv header */
	xio_mbuf_set_val_start(&task->mbuf);

	/* jump after connection setup header */
	if (tcp_hndl->base.is_client)
		xio_mbuf_inc(&task->mbuf,
			     sizeof(struct xio_nexus_setup_req));
	else
		xio_mbuf_inc(&task->mbuf,
			     sizeof(struct xio_nexus_setup_rsp));

	tmp_msg = xio_mbuf_get_curr_ptr(&task->mbuf);

	/* pack relevant values */
	PACK_LLVAL(msg, tmp_msg, buffer_sz);
	PACK_LVAL(msg, tmp_msg, max_in_iovsz);
	PACK_LVAL(msg, tmp_msg, max_out_iovsz);

#ifdef EYAL_TODO
	print_hex_dump_bytes("post_send: ", DUMP_PREFIX_ADDRESS,
			     task->mbuf.tlv.head,
			     64);
#endif
	xio_mbuf_inc(&task->mbuf, sizeof(struct xio_tcp_setup_msg));
}

/*---------------------------------------------------------------------------*/
/* xio_rdma_read_setup_msg						     */
/*---------------------------------------------------------------------------*/
static void xio_tcp_read_setup_msg(struct xio_tcp_transport *tcp_hndl,
				   struct xio_task *task,
				   struct xio_tcp_setup_msg *msg)
{
	struct xio_tcp_setup_msg	*tmp_msg;

	/* set the mbuf after tlv header */
	xio_mbuf_set_val_start(&task->mbuf);

	/* jump after connection setup header */
	if (tcp_hndl->base.is_client)
		xio_mbuf_inc(&task->mbuf,
			     sizeof(struct xio_nexus_setup_rsp));
	else
		xio_mbuf_inc(&task->mbuf,
			     sizeof(struct xio_nexus_setup_req));

	tmp_msg = xio_mbuf_get_curr_ptr(&task->mbuf);

	/* pack relevant values */
	UNPACK_LLVAL(tmp_msg, msg, buffer_sz);
	UNPACK_LVAL(tmp_msg, msg, max_in_iovsz);
	UNPACK_LVAL(tmp_msg, msg, max_out_iovsz);

#ifdef EYAL_TODO
	print_hex_dump_bytes("post_send: ", DUMP_PREFIX_ADDRESS,
			     task->mbuf.curr,
			     64);
#endif
	xio_mbuf_inc(&task->mbuf, sizeof(struct xio_tcp_setup_msg));
}


/*---------------------------------------------------------------------------*/
/* xio_tcp_send_setup_req						     */
/*---------------------------------------------------------------------------*/
static int xio_tcp_send_setup_req(struct xio_tcp_transport *tcp_hndl,
				  struct xio_task *task)
{
	uint16_t payload;
	XIO_TO_TCP_TASK(task, tcp_task);
	struct xio_tcp_setup_msg  req;

	DEBUG_LOG("xio_tcp_send_setup_req\n");

	req.buffer_sz		= tcp_hndl->max_send_buf_sz;
	req.max_in_iovsz	= tcp_options.max_in_iovsz;
	req.max_out_iovsz	= tcp_options.max_out_iovsz;

	xio_tcp_write_setup_msg(tcp_hndl, task, &req);

	payload = xio_mbuf_tlv_payload_len(&task->mbuf);

	/* add tlv */
	if (xio_mbuf_write_tlv(&task->mbuf, task->tlv_type, payload) != 0)
		return  -1;

	TRACE_LOG("tcp send setup request\n");

	/* set the length */
	tcp_task->txd.msg_iov[0].iov_len = xio_mbuf_data_length(&task->mbuf);
	tcp_task->txd.msg_len		 = 1;
	tcp_task->txd.tot_iov_byte_len	 = tcp_task->txd.msg_iov[0].iov_len;
	tcp_task->txd.msg.msg_iov	 = tcp_task->txd.msg_iov;
	tcp_task->txd.msg.msg_iovlen	 = tcp_task->txd.msg_len;

	tcp_task->tcp_op		 = XIO_TCP_SEND;

	xio_task_addref(task);

	xio_tcp_send_work(tcp_hndl, &tcp_task->txd, 1);

	list_move_tail(&task->tasks_list_entry, &tcp_hndl->in_flight_list);

	return 0;
}

/*---------------------------------------------------------------------------*/
/* xio_rdma_send_setup_rsp						     */
/*---------------------------------------------------------------------------*/
static int xio_tcp_send_setup_rsp(struct xio_tcp_transport *tcp_hndl,
				  struct xio_task *task)
{
	uint16_t payload;
	XIO_TO_TCP_TASK(task, tcp_task);
	struct xio_tcp_setup_msg *rsp = &tcp_hndl->setup_rsp;

	DEBUG_LOG("xio_tcp_send_setup_rsp\n");

	rsp->max_in_iovsz	= tcp_options.max_in_iovsz;
	rsp->max_out_iovsz	= tcp_options.max_out_iovsz;

	xio_tcp_write_setup_msg(tcp_hndl, task, rsp);

	payload = xio_mbuf_tlv_payload_len(&task->mbuf);

	/* add tlv */
	if (xio_mbuf_write_tlv(&task->mbuf, task->tlv_type, payload) != 0)
		return  -1;

	TRACE_LOG("tcp send setup response\n");

	/* set the length */
	tcp_task->txd.msg_iov[0].iov_len = xio_mbuf_data_length(&task->mbuf);
	tcp_task->txd.msg_len		 = 1;
	tcp_task->txd.tot_iov_byte_len	 = tcp_task->txd.msg_iov[0].iov_len;
	tcp_task->txd.msg.msg_iov	 = tcp_task->txd.msg_iov;
	tcp_task->txd.msg.msg_iovlen	 = tcp_task->txd.msg_len;

	tcp_task->tcp_op		 = XIO_TCP_SEND;

	xio_tcp_send_work(tcp_hndl, &tcp_task->txd, 1);

	list_move(&task->tasks_list_entry, &tcp_hndl->in_flight_list);

	return 0;
}

/*---------------------------------------------------------------------------*/
/* xio_rdma_on_setup_msg						     */
/*---------------------------------------------------------------------------*/
static int xio_tcp_on_setup_msg(struct xio_tcp_transport *tcp_hndl,
				struct xio_task *task)
{
	union xio_transport_event_data event_data;
	struct xio_tcp_setup_msg *rsp  = &tcp_hndl->setup_rsp;

	DEBUG_LOG("xio_tcp_on_setup_msg\n");

	if (tcp_hndl->base.is_client) {
		struct xio_task *sender_task = NULL;
		if (!list_empty(&tcp_hndl->in_flight_list))
			sender_task = list_first_entry(
					&tcp_hndl->in_flight_list,
					struct xio_task,  tasks_list_entry);
		else if (!list_empty(&tcp_hndl->tx_comp_list))
			sender_task = list_first_entry(
					&tcp_hndl->tx_comp_list,
					struct xio_task,  tasks_list_entry);
		else
			ERROR_LOG("could not find sender task\n");

		task->sender_task = sender_task;
		xio_tcp_read_setup_msg(tcp_hndl, task, rsp);
	} else {
		struct xio_tcp_setup_msg req;

		xio_tcp_read_setup_msg(tcp_hndl, task, &req);

		/* current implementation is symmetric */
		rsp->buffer_sz	= min(req.buffer_sz,
				tcp_hndl->max_send_buf_sz);
		rsp->max_in_iovsz	= req.max_in_iovsz;
		rsp->max_out_iovsz	= req.max_out_iovsz;
	}

	tcp_hndl->max_send_buf_sz	= rsp->buffer_sz;
	tcp_hndl->membuf_sz		= rsp->buffer_sz;
	tcp_hndl->peer_max_in_iovsz	= rsp->max_in_iovsz;
	tcp_hndl->peer_max_out_iovsz	= rsp->max_out_iovsz;

	/* now we can calculate  primary pool size */
	xio_tcp_calc_pool_size(tcp_hndl);

	tcp_hndl->state = XIO_STATE_CONNECTED;

	/* fill notification event */
	event_data.msg.op	= XIO_WC_OP_RECV;
	event_data.msg.task	= task;

	list_move_tail(&task->tasks_list_entry, &tcp_hndl->io_list);

	xio_transport_notify_observer(&tcp_hndl->base,
				      XIO_TRANSPORT_NEW_MESSAGE, &event_data);
	return 0;
}

/*---------------------------------------------------------------------------*/
/* xio_tcp_write_req_header						     */
/*---------------------------------------------------------------------------*/
static int xio_tcp_write_req_header(struct xio_tcp_transport *tcp_hndl,
				    struct xio_task *task,
				    struct xio_tcp_req_hdr *req_hdr)
{
	struct xio_tcp_req_hdr		*tmp_req_hdr;
	struct xio_sge			*tmp_sge;
	struct xio_sge			sge;
	size_t				hdr_len;
	uint32_t			i;
	XIO_TO_TCP_TASK(task, tcp_task);


	/* point to transport header */
	xio_mbuf_set_trans_hdr(&task->mbuf);
	tmp_req_hdr = xio_mbuf_get_curr_ptr(&task->mbuf);

	/* pack relevant values */
	tmp_req_hdr->version  = req_hdr->version;
	tmp_req_hdr->flags    = req_hdr->flags;
	PACK_SVAL(req_hdr, tmp_req_hdr, req_hdr_len);
	PACK_SVAL(req_hdr, tmp_req_hdr, tid);
	tmp_req_hdr->opcode	   = req_hdr->opcode;

	PACK_SVAL(req_hdr, tmp_req_hdr, recv_num_sge);
	PACK_SVAL(req_hdr, tmp_req_hdr, read_num_sge);
	PACK_SVAL(req_hdr, tmp_req_hdr, write_num_sge);
	PACK_SVAL(req_hdr, tmp_req_hdr, ulp_hdr_len);
	PACK_SVAL(req_hdr, tmp_req_hdr, ulp_pad_len);
	/*remain_data_len is not used		*/
	PACK_LLVAL(req_hdr, tmp_req_hdr, ulp_imm_len);

	tmp_sge = (void *)((uint8_t *)tmp_req_hdr +
			   sizeof(struct xio_tcp_req_hdr));

	/* IN: requester expect small input written via send */
	for (i = 0;  i < req_hdr->recv_num_sge; i++) {
		sge.addr = 0;
		sge.length = task->omsg->in.pdata_iov[i].iov_len;
		sge.stag = 0;
		PACK_LLVAL(&sge, tmp_sge, addr);
		PACK_LVAL(&sge, tmp_sge, length);
		PACK_LVAL(&sge, tmp_sge, stag);
		tmp_sge++;
	}
	/* IN: requester expect big input written rdma write */
	for (i = 0;  i < req_hdr->read_num_sge; i++) {
		sge.addr = uint64_from_ptr(tcp_task->read_sge[i].addr);
		sge.length = tcp_task->read_sge[i].length;
		sge.stag = 0;
		PACK_LLVAL(&sge, tmp_sge, addr);
		PACK_LVAL(&sge, tmp_sge, length);
		PACK_LVAL(&sge, tmp_sge, stag);
		tmp_sge++;
	}
	/* OUT: requester want to write data via rdma read */
	for (i = 0;  i < req_hdr->write_num_sge; i++) {
		sge.addr = uint64_from_ptr(tcp_task->write_sge[i].addr);
		sge.length = tcp_task->write_sge[i].length;
		sge.stag = 0;
		PACK_LLVAL(&sge, tmp_sge, addr);
		PACK_LVAL(&sge, tmp_sge, length);
		PACK_LVAL(&sge, tmp_sge, stag);
		tmp_sge++;
	}

	hdr_len	= sizeof(struct xio_tcp_req_hdr);
	hdr_len += sizeof(struct xio_sge)*(req_hdr->recv_num_sge +
						   req_hdr->read_num_sge +
						   req_hdr->write_num_sge);
#ifdef EYAL_TODO
	print_hex_dump_bytes("post_send: ", DUMP_PREFIX_ADDRESS,
			     task->mbuf.curr,
			     hdr_len + 16);
#endif
	xio_mbuf_inc(&task->mbuf, hdr_len);

	return 0;
}

/*---------------------------------------------------------------------------*/
/* xio_tcp_prep_req_header						     */
/*---------------------------------------------------------------------------*/
static int xio_tcp_prep_req_header(struct xio_tcp_transport *tcp_hndl,
				   struct xio_task *task,
				   uint16_t ulp_hdr_len,
				   uint16_t ulp_pad_len,
				   uint64_t ulp_imm_len,
				   uint32_t status)
{
	struct xio_tcp_req_hdr	req_hdr;
	XIO_TO_TCP_TASK(task, tcp_task);

	if (!IS_REQUEST(task->tlv_type)) {
		ERROR_LOG("unknown message type\n");
		return -1;
	}

	/* write the headers */

	/* fill request header */
	req_hdr.version		= XIO_TCP_REQ_HEADER_VERSION;
	req_hdr.req_hdr_len	= sizeof(req_hdr);
	req_hdr.tid		= task->ltid;
	req_hdr.opcode		= tcp_task->tcp_op;
	if (task->omsg_flags & XIO_MSG_FLAG_SMALL_ZERO_COPY)
		req_hdr.flags = XIO_HEADER_FLAG_SMALL_ZERO_COPY;
	else
		req_hdr.flags = XIO_HEADER_FLAG_NONE;
	req_hdr.ulp_hdr_len	= ulp_hdr_len;
	req_hdr.ulp_pad_len	= ulp_pad_len;
	req_hdr.ulp_imm_len	= ulp_imm_len;
	req_hdr.recv_num_sge	= tcp_task->recv_num_sge;
	req_hdr.read_num_sge	= tcp_task->read_num_sge;
	req_hdr.write_num_sge	= tcp_task->write_num_sge;

	if (xio_tcp_write_req_header(tcp_hndl, task, &req_hdr) != 0)
		goto cleanup;

	/* write the payload header */
	if (ulp_hdr_len) {
		if (xio_mbuf_write_array(
		    &task->mbuf,
		    task->omsg->out.header.iov_base,
		    task->omsg->out.header.iov_len) != 0)
			goto cleanup;
	}

	/* write the pad between header and data */
	if (ulp_pad_len)
		xio_mbuf_inc(&task->mbuf, ulp_pad_len);

	return 0;

cleanup:
	xio_set_error(XIO_E_MSG_SIZE);
	ERROR_LOG("xio_rdma_write_req_header failed\n");
	return -1;
}

/*---------------------------------------------------------------------------*/
/* xio_tcp_write_send_data						     */
/*---------------------------------------------------------------------------*/
static int xio_tcp_write_send_data(
		struct xio_tcp_transport *tcp_hndl,
		struct xio_task *task)
{
	XIO_TO_TCP_TASK(task, tcp_task);
	size_t			i;
	size_t			byte_len = 0;
	struct xio_iovec_ex *iov =
			&task->omsg->out.pdata_iov[0];

	/* user provided mr */
	if (task->omsg->out.pdata_iov[0].mr) {
		for (i = 0; i < task->omsg->out.data_iovlen; i++)  {
			tcp_task->txd.msg_iov[i+1].iov_base = iov->iov_base;
			tcp_task->txd.msg_iov[i+1].iov_len = iov->iov_len;
			byte_len += iov->iov_len;
			iov++;
		}
		tcp_task->txd.msg_len = task->omsg->out.data_iovlen + 1;
		tcp_task->txd.tot_iov_byte_len = byte_len;
	} else {
		/* copy to internal buffer */
		for (i = 0; i < task->omsg->out.data_iovlen; i++) {
			/* copy the data into internal buffer */
			if (xio_mbuf_write_array(
				&task->mbuf,
				task->omsg->out.pdata_iov[i].iov_base,
				task->omsg->out.pdata_iov[i].iov_len) != 0)
				goto cleanup;
		}
		tcp_task->txd.msg_len = 1;
		tcp_task->txd.tot_iov_byte_len = 0;
	}

	return 0;

cleanup:
	xio_set_error(XIO_E_MSG_SIZE);
	ERROR_LOG("xio_tcp_send_msg failed\n");
	return -1;
}

/*---------------------------------------------------------------------------*/
/* xio_tcp_prep_req_out_data						     */
/*---------------------------------------------------------------------------*/
static int xio_tcp_prep_req_out_data(
		struct xio_tcp_transport *tcp_hndl,
		struct xio_task *task)
{
	XIO_TO_TCP_TASK(task, tcp_task);
	struct xio_vmsg		*vmsg = &task->omsg->out;
	uint64_t		xio_hdr_len;
	uint64_t		ulp_out_hdr_len;
	uint64_t		ulp_pad_len = 0;
	uint64_t		ulp_out_imm_len;
	size_t			retval;
	int			i;

	/* calculate headers */
	ulp_out_hdr_len	= vmsg->header.iov_len;
	ulp_out_imm_len	= xio_iovex_length(vmsg->pdata_iov,
					   vmsg->data_iovlen);

	xio_hdr_len = xio_mbuf_get_curr_offset(&task->mbuf);
	xio_hdr_len += sizeof(struct xio_tcp_req_hdr);
	xio_hdr_len += sizeof(struct xio_sge)*(tcp_task->recv_num_sge +
					       tcp_task->read_num_sge +
					       vmsg->data_iovlen);

	if (tcp_hndl->max_send_buf_sz	 < (xio_hdr_len + ulp_out_hdr_len)) {
		ERROR_LOG("header size %lu exceeds max header %lu\n",
			  ulp_out_hdr_len, tcp_hndl->max_send_buf_sz -
			  xio_hdr_len);
		xio_set_error(XIO_E_MSG_SIZE);
		return -1;
	}


	/* the data is outgoing via SEND */
	if ((ulp_out_hdr_len + ulp_out_imm_len +
			xio_hdr_len) < tcp_hndl->max_send_buf_sz) {
		tcp_task->tcp_op = XIO_TCP_SEND;
		/* user has small request - no rdma operation expected */
		tcp_task->write_num_sge = 0;

		/* write xio header to the buffer */
		retval = xio_tcp_prep_req_header(
				tcp_hndl, task,
				ulp_out_hdr_len, ulp_pad_len, ulp_out_imm_len,
				XIO_E_SUCCESS);
		if (retval)
			return -1;

		/* if there is data, set it to buffer or directly to the sge */
		if (ulp_out_imm_len) {
			retval = xio_tcp_write_send_data(tcp_hndl, task);
			if (retval)
				return -1;
		} else {
			tcp_task->txd.tot_iov_byte_len = 0;
			tcp_task->txd.msg_len = 1;
		}
	} else {
		tcp_task->tcp_op = XIO_TCP_READ;
		if (task->omsg->out.pdata_iov[0].mr) {
			for (i = 0; i < vmsg->data_iovlen; i++) {
				tcp_task->write_sge[i].addr =
						vmsg->pdata_iov[i].iov_base;
				tcp_task->write_sge[i].cache = NULL;
				tcp_task->write_sge[i].mr =
					task->omsg->out.pdata_iov[i].mr;
				tcp_task->write_sge[i].length =
						vmsg->pdata_iov[i].iov_len;
			}
		} else {
			if (tcp_hndl->tcp_mempool == NULL) {
				xio_set_error(XIO_E_NO_BUFS);
				ERROR_LOG("message /read/write failed - " \
					  "library's memory pool disabled\n");
				goto cleanup;
			}

			/* user did not provide mr - take buffers from pool
			 * and do copy */
			for (i = 0; i < vmsg->data_iovlen; i++) {
				retval = xio_mempool_alloc(
						tcp_hndl->tcp_mempool,
						vmsg->pdata_iov[i].iov_len,
						&tcp_task->write_sge[i]);
				if (retval) {
					tcp_task->write_num_sge = i;
					xio_set_error(ENOMEM);
					ERROR_LOG("mempool is empty " \
						  "for %zd bytes\n",
						  vmsg->pdata_iov[i].iov_len);
					goto cleanup;
				}

				tcp_task->write_sge[i].length =
						vmsg->pdata_iov[i].iov_len;

				/* copy the data to the buffer */
				memcpy(tcp_task->write_sge[i].addr,
				       vmsg->pdata_iov[i].iov_base,
				       vmsg->pdata_iov[i].iov_len);
			}
		}
		tcp_task->write_num_sge = vmsg->data_iovlen;

		if (ulp_out_imm_len) {
			tcp_task->txd.tot_iov_byte_len = 0;
			for (i = 0; i < vmsg->data_iovlen; i++)  {
				tcp_task->txd.msg_iov[i+1].iov_base =
						tcp_task->write_sge[i].addr;
				tcp_task->txd.msg_iov[i+1].iov_len =
						tcp_task->write_sge[i].length;
				tcp_task->txd.tot_iov_byte_len +=
						tcp_task->write_sge[i].length;
			}
			tcp_task->txd.msg_len = vmsg->data_iovlen + 1;
		} else {
			tcp_task->txd.tot_iov_byte_len = 0;
			tcp_task->txd.msg_len = 1;
		}

		/* write xio header to the buffer */
		retval = xio_tcp_prep_req_header(
				tcp_hndl, task,
				ulp_out_hdr_len, 0, 0, XIO_E_SUCCESS);

		if (retval) {
			ERROR_LOG("Failed to write header\n");
			goto cleanup;
		}
	}

	return 0;

cleanup:
	for (i = 0; i < tcp_task->write_num_sge; i++)
		xio_mempool_free(&tcp_task->write_sge[i]);

	tcp_task->write_num_sge = 0;

	return -1;
}

/*---------------------------------------------------------------------------*/
/* xio_tcp_on_rsp_send_comp						     */
/*---------------------------------------------------------------------------*/
static int xio_tcp_on_rsp_send_comp(struct xio_tcp_transport *tcp_hndl,
				    struct xio_task *task)
{
	union xio_transport_event_data event_data;

	if (IS_CANCEL(task->tlv_type))
		return 0;

	event_data.msg.op	= XIO_WC_OP_SEND;
	event_data.msg.task	= task;

	xio_transport_notify_observer(&tcp_hndl->base,
				      XIO_TRANSPORT_SEND_COMPLETION,
				      &event_data);

	return 0;
}

/*---------------------------------------------------------------------------*/
/* xio_tcp_on_req_send_comp						     */
/*---------------------------------------------------------------------------*/
static int xio_tcp_on_req_send_comp(struct xio_tcp_transport *tcp_hndl,
				    struct xio_task *task)
{
	union xio_transport_event_data event_data;

	if (IS_CANCEL(task->tlv_type))
		return 0;

	event_data.msg.op	= XIO_WC_OP_SEND;
	event_data.msg.task	= task;

	xio_transport_notify_observer(&tcp_hndl->base,
				      XIO_TRANSPORT_SEND_COMPLETION,
				      &event_data);

	return 0;
}

/*---------------------------------------------------------------------------*/
/* xio_tcp_tx_comp_handler						     */
/*---------------------------------------------------------------------------*/
static void xio_tcp_tx_completion_handler(void *xio_task)
{
	struct xio_task		*task, *ptask, *next_ptask;
	struct xio_tcp_task	*tcp_task;
	struct xio_tcp_transport *tcp_hndl;
	int			found = 0;
	int			removed = 0;

	task = xio_task;
	tcp_task = task->dd_data;
	tcp_hndl = tcp_task->tcp_hndl;

	list_for_each_entry_safe(ptask, next_ptask, &tcp_hndl->in_flight_list,
				 tasks_list_entry) {
		list_move_tail(&ptask->tasks_list_entry,
			       &tcp_hndl->tx_comp_list);
		removed++;
		tcp_task = ptask->dd_data;

		if (IS_REQUEST(ptask->tlv_type)) {
			xio_tcp_on_req_send_comp(tcp_hndl, ptask);
			xio_tasks_pool_put(ptask);
		} else if (IS_RESPONSE(ptask->tlv_type)) {
			xio_tcp_on_rsp_send_comp(tcp_hndl, ptask);
		} else {
			ERROR_LOG("unexpected task %p id:%d magic:0x%lx\n",
				  ptask,
				  ptask->ltid, ptask->magic);
			continue;
		}
		if (ptask == task) {
			found  = 1;
			break;
		}
	}

	if (tcp_hndl->tx_ready_tasks_num)
		xio_tcp_xmit(tcp_hndl);


	if (!found && removed)
		ERROR_LOG("not found but removed %d type:0x%x\n",
			  removed, task->tlv_type);
}

/*
static void xio_tcp_xmit_handler(void *xio_task)
{
	struct xio_task		*task;
	struct xio_tcp_task	*tcp_task;

	task = xio_task;
	tcp_task = task->dd_data;
	xio_tcp_xmit(tcp_task->tcp_hndl);
}*/

/*---------------------------------------------------------------------------*/
/* xio_tcp_xmit							     */
/*---------------------------------------------------------------------------*/
static int xio_tcp_xmit(struct xio_tcp_transport *tcp_hndl)
{
	struct xio_task		*task = NULL, *task_success = NULL,
				*task1, *task2;
	struct xio_tcp_task	*tcp_task = NULL;
	int			retval;

	/* if "ready to send queue" is not empty */
	while (tcp_hndl->tx_ready_tasks_num) {
		task = list_first_entry(
				&tcp_hndl->tx_ready_list,
				struct xio_task,  tasks_list_entry);

		tcp_task = task->dd_data;

		/* prefetch next buffer */
		if (tcp_hndl->tx_ready_tasks_num > 2) {
			task1 = list_first_entry_or_null(
					&task->tasks_list_entry,
					struct xio_task,  tasks_list_entry);
			if (task1) {
				xio_prefetch(task1->mbuf.buf.head);
				task2 = list_first_entry_or_null(
						&task1->tasks_list_entry,
						struct xio_task,
						tasks_list_entry);
				if (task2)
					xio_prefetch(task2->mbuf.buf.head);
			}
		}

		/* ORK todo batch? */
		retval = xio_tcp_send_work(tcp_hndl, &tcp_task->txd, 1);

		if (retval < 0) {
			/* ORK todo add event (not work!) for ready for write*/
			/*retval = xio_ctx_add_work(tcp_hndl->base.ctx,
						    task,
						    xio_tcp_xmit_handler,
						    &tcp_task->xmit_work);
			if (retval != 0) {
				ERROR_LOG("xio_ctx_add_work failed.\n");
				return retval;
			}*/

			if (errno == ECONNRESET) {
				TRACE_LOG("tcp trans got reset, tcp_hndl=%p\n",
					  tcp_hndl);
				on_sock_disconnected(tcp_hndl, 1);
			}

			return -1;
		}

		tcp_hndl->tx_ready_tasks_num--;

		list_move_tail(&task->tasks_list_entry,
			       &tcp_hndl->in_flight_list);

		task_success = task;

		++tcp_hndl->tx_comp_cnt;
	}

	/* ORK todo 1 make sure to remove the handle on close, when?*/
	/* ORK todo 2 batch completions*/
	if (task_success &&
	    (tcp_hndl->tx_comp_cnt >= COMPLETION_BATCH_MAX ||
	     task->is_control ||
	     (task->omsg->flags & XIO_MSG_FLAG_IMM_SEND_COMP))) {
		retval = xio_ctx_add_work(tcp_hndl->base.ctx,
					  task_success,
					  xio_tcp_tx_completion_handler,
					  &tcp_task->comp_work);
		if (retval != 0) {
			ERROR_LOG("xio_ctx_add_work failed.\n");
			return retval;
		}

		tcp_hndl->tx_comp_cnt = 0;
	}

	return 0;
}

/*---------------------------------------------------------------------------*/
/* xio_tcp_prep_req_in_data						     */
/*---------------------------------------------------------------------------*/
static int xio_tcp_prep_req_in_data(struct xio_tcp_transport *tcp_hndl,
				    struct xio_task *task)
{
	XIO_TO_TCP_TASK(task, tcp_task);
	size_t				hdr_len;
	size_t				data_len;
	struct xio_vmsg			*vmsg = &task->omsg->in;
	int				i;
	int				retval;


	if (vmsg->data_iovlen == 0) {
		tcp_task->recv_num_sge = 0;
		tcp_task->read_num_sge = 0;
		return 0;
	}

	data_len  = xio_iovex_length(vmsg->pdata_iov, vmsg->data_iovlen);
	hdr_len  = vmsg->header.iov_len;

	/* requester may insist on RDMA for small buffers to eliminate copy
	 * from receive buffers to user buffers
	 */
	if (!(task->omsg_flags & XIO_MSG_FLAG_SMALL_ZERO_COPY) &&
	    data_len + hdr_len + MAX_HDR_SZ < tcp_hndl->max_send_buf_sz) {
		/* user has small response - no rdma operation expected */
		tcp_task->read_num_sge = 0;
		if (data_len)
			tcp_task->recv_num_sge = vmsg->data_iovlen;
	} else  {
		/* user provided buffers with length for RDMA WRITE */
		/* user provided mr */
		if (vmsg->pdata_iov[0].mr) {
			for (i = 0; i < vmsg->data_iovlen; i++) {
				tcp_task->read_sge[i].addr =
					vmsg->pdata_iov[i].iov_base;
				tcp_task->read_sge[i].cache = NULL;
				tcp_task->read_sge[i].mr =
					vmsg->pdata_iov[i].mr;
				tcp_task->read_sge[i].length =
					vmsg->pdata_iov[i].iov_len;
			}
		} else {
			if (tcp_hndl->tcp_mempool == NULL) {
				xio_set_error(XIO_E_NO_BUFS);
				ERROR_LOG("message /read/write failed - "
					  "library's memory pool disabled\n");
				goto cleanup;
			}

			/* user did not provide mr */
			for (i = 0; i < vmsg->data_iovlen; i++) {
				retval = xio_mempool_alloc(
						tcp_hndl->tcp_mempool,
						vmsg->pdata_iov[i].iov_len,
						&tcp_task->read_sge[i]);

				if (retval) {
					tcp_task->read_num_sge = i;
					xio_set_error(ENOMEM);
					ERROR_LOG(
					"mempool is empty for %zd bytes\n",
					vmsg->pdata_iov[i].iov_len);
					goto cleanup;
				}
				tcp_task->read_sge[i].length =
					vmsg->pdata_iov[i].iov_len;
			}
		}
		tcp_task->read_num_sge = vmsg->data_iovlen;
		tcp_task->recv_num_sge = 0;
	}
	if (tcp_task->read_num_sge > tcp_hndl->peer_max_out_iovsz) {
		ERROR_LOG("request in iovlen %d is bigger "
			  "than peer max out iovlen %d\n",
			  tcp_task->read_num_sge,
			  tcp_hndl->peer_max_out_iovsz);
		goto cleanup;
	}

	return 0;

cleanup:
	for (i = 0; i < tcp_task->read_num_sge; i++)
		xio_mempool_free(&tcp_task->read_sge[i]);

	tcp_task->read_num_sge = 0;
	tcp_task->recv_num_sge = 0;
	xio_set_error(EMSGSIZE);

	return -1;
}

/*---------------------------------------------------------------------------*/
/* xio_tcp_send_req							     */
/*---------------------------------------------------------------------------*/
static int xio_tcp_send_req(struct xio_tcp_transport *tcp_hndl,
			    struct xio_task *task)
{
	XIO_TO_TCP_TASK(task, tcp_task);
	uint64_t		payload;
	size_t			retval;
	int			must_send = 0;
	size_t			iov_len;
	size_t			tlv_len;

	/* prepare buffer for response  */
	retval = xio_tcp_prep_req_in_data(tcp_hndl, task);
	if (retval != 0) {
		ERROR_LOG("tcp_prep_req_in_data failed\n");
		return -1;
	}

	/* prepare the out message  */
	retval = xio_tcp_prep_req_out_data(tcp_hndl, task);
	if (retval != 0) {
		ERROR_LOG("tcp_prep_req_out_data failed\n");
		return -1;
	}

	payload = xio_mbuf_tlv_payload_len(&task->mbuf);

	/* set the length */
	iov_len = xio_mbuf_get_curr_offset(&task->mbuf);
	tcp_task->txd.msg_iov[0].iov_len = iov_len;

	tlv_len = iov_len - XIO_TLV_LEN;
	if (tcp_task->tcp_op == XIO_TCP_SEND)
		tlv_len += tcp_task->txd.tot_iov_byte_len;

	tcp_task->txd.tot_iov_byte_len += iov_len;

	tcp_task->txd.msg.msg_iov = tcp_task->txd.msg_iov;
	tcp_task->txd.msg.msg_iovlen = tcp_task->txd.msg_len;

	/* validate header */
	if (XIO_TLV_LEN + payload != iov_len) {
		ERROR_LOG("header validation failed\n");
		return -1;
	}

	/* add tlv */
	if (xio_mbuf_write_tlv(&task->mbuf, task->tlv_type, tlv_len) != 0) {
		ERROR_LOG("write tlv failed\n");
		xio_set_error(EOVERFLOW);
		return -1;
	}

	xio_task_addref(task);

	tcp_task->tcp_op = XIO_TCP_SEND;

	list_move_tail(&task->tasks_list_entry, &tcp_hndl->tx_ready_list);

	tcp_hndl->tx_ready_tasks_num++;

	/* transmit only if  available */
	if (task->omsg->more_in_batch == 0) {
		must_send = 1;
	} else {
		/* ORK todo add logic for batching sends*/
		must_send = 1;
	}

	if (must_send) {
		retval = xio_tcp_xmit(tcp_hndl);
		if (retval) {
			if (xio_errno() != EAGAIN) {
				ERROR_LOG("xio_tcp_xmit failed\n");
				return -1;
			}
			retval = 0;
		}
	}

	return retval;
}

/*---------------------------------------------------------------------------*/
/* xio_tcp_write_rsp_header						     */
/*---------------------------------------------------------------------------*/
static int xio_tcp_write_rsp_header(struct xio_tcp_transport *tcp_hndl,
				    struct xio_task *task,
				    struct xio_tcp_rsp_hdr *rsp_hdr)
{
	struct xio_tcp_rsp_hdr		*tmp_rsp_hdr;
	uint32_t			*wr_len;
	int				i;
	size_t				hdr_len;
	XIO_TO_TCP_TASK(task, tcp_task);

	/* point to transport header */
	xio_mbuf_set_trans_hdr(&task->mbuf);
	tmp_rsp_hdr = xio_mbuf_get_curr_ptr(&task->mbuf);

	/* pack relevant values */
	tmp_rsp_hdr->version  = rsp_hdr->version;
	tmp_rsp_hdr->flags    = rsp_hdr->flags;
	PACK_SVAL(rsp_hdr, tmp_rsp_hdr, rsp_hdr_len);
	PACK_SVAL(rsp_hdr, tmp_rsp_hdr, tid);
	tmp_rsp_hdr->opcode = rsp_hdr->opcode;
	PACK_LVAL(rsp_hdr, tmp_rsp_hdr, status);
	PACK_SVAL(rsp_hdr, tmp_rsp_hdr, write_num_sge);
	PACK_SVAL(rsp_hdr, tmp_rsp_hdr, ulp_hdr_len);
	PACK_SVAL(rsp_hdr, tmp_rsp_hdr, ulp_pad_len);
	/* remain_data_len not in use */
	PACK_LLVAL(rsp_hdr, tmp_rsp_hdr, ulp_imm_len);

	if (rsp_hdr->write_num_sge) {
		wr_len = (uint32_t *)((uint8_t *)tmp_rsp_hdr +
				sizeof(struct xio_tcp_rsp_hdr));

		/* params for RDMA WRITE equivalent*/
		for (i = 0;  i < rsp_hdr->write_num_sge; i++) {
			*wr_len = htonl(tcp_task->rsp_write_sge[i].length);
			wr_len++;
		}
	}

	hdr_len	= sizeof(struct xio_tcp_rsp_hdr);
	hdr_len += sizeof(uint32_t)*rsp_hdr->write_num_sge;

	xio_mbuf_inc(&task->mbuf, hdr_len);

#ifdef EYAL_TODO
	print_hex_dump_bytes("post_send: ", DUMP_PREFIX_ADDRESS,
			     task->mbuf.tlv.head, 64);
#endif
	return 0;
}

/*---------------------------------------------------------------------------*/
/* xio_tcp_prep_rsp_header						     */
/*---------------------------------------------------------------------------*/
static int xio_tcp_prep_rsp_header(struct xio_tcp_transport *tcp_hndl,
				   struct xio_task *task,
				   uint16_t ulp_hdr_len,
				   uint16_t ulp_pad_len,
				   uint64_t ulp_imm_len,
				   uint32_t status)
{
	XIO_TO_TCP_TASK(task, tcp_task);
	struct xio_tcp_rsp_hdr	rsp_hdr;

	if (!IS_RESPONSE(task->tlv_type)) {
		ERROR_LOG("unknown message type\n");
		return -1;
	}

	/* fill response header */
	rsp_hdr.version		= XIO_TCP_RSP_HEADER_VERSION;
	rsp_hdr.rsp_hdr_len	= sizeof(rsp_hdr);
	rsp_hdr.tid		= task->rtid;
	rsp_hdr.opcode		= tcp_task->tcp_op;
	rsp_hdr.flags		= XIO_HEADER_FLAG_NONE;
	rsp_hdr.write_num_sge	= tcp_task->rsp_write_num_sge;
	rsp_hdr.ulp_hdr_len	= ulp_hdr_len;
	rsp_hdr.ulp_pad_len	= ulp_pad_len;
	rsp_hdr.ulp_imm_len	= ulp_imm_len;
	rsp_hdr.status		= status;
	if (xio_tcp_write_rsp_header(tcp_hndl, task, &rsp_hdr) != 0)
		goto cleanup;

	/* write the payload header */
	if (ulp_hdr_len) {
		if (xio_mbuf_write_array(
		    &task->mbuf,
		    task->omsg->out.header.iov_base,
		    task->omsg->out.header.iov_len) != 0)
			goto cleanup;
	}

	/* write the pad between header and data */
	if (ulp_pad_len)
		xio_mbuf_inc(&task->mbuf, ulp_pad_len);

	return 0;

cleanup:
	xio_set_error(XIO_E_MSG_SIZE);
	ERROR_LOG("xio_tcp_write_rsp_header failed\n");
	return -1;
}

/*---------------------------------------------------------------------------*/
/* xio_tcp_prep_rsp_wr_data						     */
/*---------------------------------------------------------------------------*/
int xio_tcp_prep_rsp_wr_data(struct xio_tcp_transport *tcp_hndl,
			     struct xio_task *task)
{
	XIO_TO_TCP_TASK(task, tcp_task);
	int i, llen = 0, rlen = 0;
	int retval;

	/* user did not provided mr */
	if (task->omsg->out.pdata_iov[0].mr == NULL) {
		if (tcp_hndl->tcp_mempool == NULL) {
			xio_set_error(XIO_E_NO_BUFS);
			ERROR_LOG("message /read/write failed - " \
				  "library's memory pool disabled\n");
			goto cleanup;
		}
		/* user did not provide mr - take buffers from pool
		 * and do copy */
		for (i = 0; i < task->omsg->out.data_iovlen; i++) {
			retval = xio_mempool_alloc(
					tcp_hndl->tcp_mempool,
					task->omsg->out.pdata_iov[i].iov_len,
					&tcp_task->write_sge[i]);
			if (retval) {
				tcp_task->write_num_sge = i;
				xio_set_error(ENOMEM);
				ERROR_LOG("mempool is empty for %zd bytes\n",
					  task->omsg->out.pdata_iov[i].iov_len);
				goto cleanup;
			}

			/* copy the data to the buffer */
			memcpy(tcp_task->write_sge[i].addr,
			       task->omsg->out.pdata_iov[i].iov_base,
			       task->omsg->out.pdata_iov[i].iov_len);

			tcp_task->txd.msg_iov[i+1].iov_base =
					tcp_task->write_sge[i].addr;
			tcp_task->txd.msg_iov[i+1].iov_len =
					task->omsg->out.pdata_iov[i].iov_len;
			llen += task->omsg->out.pdata_iov[i].iov_len;
		}
	} else {
		for (i = 0; i < task->omsg->out.data_iovlen; i++)  {
			tcp_task->txd.msg_iov[i+1].iov_base =
					task->omsg->out.pdata_iov[i].iov_base;
			tcp_task->txd.msg_iov[i+1].iov_len =
					task->omsg->out.pdata_iov[i].iov_len;
			llen += task->omsg->out.pdata_iov[i].iov_len;
		}
	}

	tcp_task->txd.msg_len = task->omsg->out.data_iovlen + 1;
	tcp_task->txd.tot_iov_byte_len = llen;

	for (i = 0;  i < tcp_task->req_read_num_sge; i++)
		rlen += tcp_task->req_read_sge[i].length;

	if (rlen  < llen) {
		ERROR_LOG("peer provided too small iovec\n");
		ERROR_LOG("tcp write is ignored\n");
		task->omsg->status = EINVAL;
		goto cleanup;
	}

	i = 0;
	while (llen) {
		if (tcp_task->req_read_sge[i].length < llen) {
			tcp_task->rsp_write_sge[i].length =
					tcp_task->req_read_sge[i].length;
		} else {
			tcp_task->rsp_write_sge[i].length =
					llen;
		}
		llen -= tcp_task->rsp_write_sge[i].length;
		++i;
	}
	tcp_task->rsp_write_num_sge = i;

	return 0;
cleanup:
	for (i = 0; i < tcp_task->write_num_sge; i++)
		xio_mempool_free(&tcp_task->write_sge[i]);

	tcp_task->write_num_sge = 0;
	return -1;
}

/*---------------------------------------------------------------------------*/
/* xio_tcp_send_rsp							     */
/*---------------------------------------------------------------------------*/
static int xio_tcp_send_rsp(struct xio_tcp_transport *tcp_hndl,
			    struct xio_task *task)
{
	XIO_TO_TCP_TASK(task, tcp_task);
	struct xio_tcp_rsp_hdr	rsp_hdr;
	uint64_t		payload;
	uint64_t		xio_hdr_len;
	uint64_t		ulp_hdr_len;
	uint64_t		ulp_pad_len = 0;
	uint64_t		ulp_imm_len;
	size_t			retval;
	int			must_send = 0;
	int			small_zero_copy;
	int			iov_len = 0, tlv_len = 0;

	/* calculate headers */
	ulp_hdr_len	= task->omsg->out.header.iov_len;
	ulp_imm_len	= xio_iovex_length(task->omsg->out.pdata_iov,
					   task->omsg->out.data_iovlen);
	xio_hdr_len = xio_mbuf_get_curr_offset(&task->mbuf);
	xio_hdr_len += sizeof(rsp_hdr);
	xio_hdr_len += tcp_task->rsp_write_num_sge*sizeof(struct xio_sge);
	small_zero_copy = task->imsg_flags & XIO_HEADER_FLAG_SMALL_ZERO_COPY;

	if (tcp_hndl->max_send_buf_sz < xio_hdr_len + ulp_hdr_len) {
		ERROR_LOG("header size %lu exceeds max header %lu\n",
			  ulp_hdr_len,
			  tcp_hndl->max_send_buf_sz - xio_hdr_len);
		xio_set_error(XIO_E_MSG_SIZE);
		goto cleanup;
	}

	/* Small data is outgoing via SEND unless the requester explicitly
	 * insisted on RDMA operation and provided resources.
	 */
	if ((ulp_imm_len == 0) || (!small_zero_copy &&
				   ((xio_hdr_len + ulp_hdr_len + ulp_imm_len)
				    < tcp_hndl->max_send_buf_sz))) {
		tcp_task->tcp_op = XIO_TCP_SEND;
		/* write xio header to the buffer */
		retval = xio_tcp_prep_rsp_header(
				tcp_hndl, task,
				ulp_hdr_len, ulp_pad_len, ulp_imm_len,
				XIO_E_SUCCESS);
		if (retval)
			goto cleanup;

		/* if there is data, set it to buffer or directly to the sge */
		if (ulp_imm_len) {
			retval = xio_tcp_write_send_data(tcp_hndl, task);
			if (retval)
				goto cleanup;
		}
	} else {
		if (tcp_task->req_read_sge[0].addr &&
		    tcp_task->req_read_sge[0].length) {
			/* the data is sent via RDMA_WRITE equivalent*/
			tcp_task->tcp_op = XIO_TCP_WRITE;
			/* prepare rdma write equivalent */
			retval = xio_tcp_prep_rsp_wr_data(tcp_hndl, task);
			if (retval)
				goto cleanup;

			/* and the header is sent via SEND */
			/* write xio header to the buffer */
			retval = xio_tcp_prep_rsp_header(
					tcp_hndl, task,
					ulp_hdr_len, 0, ulp_imm_len,
					XIO_E_SUCCESS);
		} else {
			ERROR_LOG("partial completion of request due " \
					"to missing, response buffer\n");

			/* the client did not provide buffer for response */
			retval = xio_tcp_prep_rsp_header(
					tcp_hndl, task,
					ulp_hdr_len, 0, 0,
					XIO_E_PARTIAL_MSG);
			goto cleanup;
		}
	}

	if (ulp_imm_len == 0) {
		/* no data at all */
		task->omsg->out.data_iovlen		= 0;
		tcp_task->txd.tot_iov_byte_len = 0;
		tcp_task->txd.msg_len = 1;
	}

	payload = xio_mbuf_tlv_payload_len(&task->mbuf);

	/* set the length */
	iov_len = xio_mbuf_get_curr_offset(&task->mbuf);
	tcp_task->txd.msg_iov[0].iov_len = iov_len;

	tlv_len = iov_len - XIO_TLV_LEN;
	if (tcp_task->tcp_op == XIO_TCP_SEND)
		tlv_len += tcp_task->txd.tot_iov_byte_len;

	tcp_task->txd.tot_iov_byte_len += iov_len;

	tcp_task->txd.msg.msg_iov = tcp_task->txd.msg_iov;
	tcp_task->txd.msg.msg_iovlen = tcp_task->txd.msg_len;

	/* validate header */
	if (XIO_TLV_LEN + payload != tcp_task->txd.msg_iov[0].iov_len) {
		ERROR_LOG("header validation failed\n");
		goto cleanup;
	}

	/* add tlv */
	if (xio_mbuf_write_tlv(&task->mbuf, task->tlv_type, tlv_len) != 0)
		goto cleanup;

	list_move_tail(&task->tasks_list_entry, &tcp_hndl->tx_ready_list);

	tcp_hndl->tx_ready_tasks_num++;

	/* transmit only if  available */
	if (task->omsg->more_in_batch == 0) {
		must_send = 1;
	} else {
		/* ORK TODO batching ? */
		must_send = 1;
	}

	if (must_send) {
		retval = xio_tcp_xmit(tcp_hndl);
		if (retval) {
			retval = xio_errno();
			if (retval != EAGAIN) {
				ERROR_LOG("xio_xmit_tcp failed. %s\n",
					  xio_strerror(retval));
				return -1;
			}
			retval = 0;
		}
	}

	return retval;

cleanup:
	xio_set_error(XIO_E_MSG_SIZE);
	ERROR_LOG("xio_tcp_send_msg failed\n");
	return -1;
}

/*---------------------------------------------------------------------------*/
/* xio_tcp_read_req_header						     */
/*---------------------------------------------------------------------------*/
static int xio_tcp_read_req_header(struct xio_tcp_transport *tcp_hndl,
				   struct xio_task *task,
				   struct xio_tcp_req_hdr *req_hdr)
{
	struct xio_tcp_req_hdr		*tmp_req_hdr;
	struct xio_sge			*tmp_sge;
	int				i;
	size_t				hdr_len;
	XIO_TO_TCP_TASK(task, tcp_task);

	/* point to transport header */
	xio_mbuf_set_trans_hdr(&task->mbuf);
	tmp_req_hdr = xio_mbuf_get_curr_ptr(&task->mbuf);


	req_hdr->version  = tmp_req_hdr->version;
	req_hdr->flags    = tmp_req_hdr->flags;
	UNPACK_SVAL(tmp_req_hdr, req_hdr, req_hdr_len);

	if (req_hdr->req_hdr_len != sizeof(struct xio_tcp_req_hdr)) {
		ERROR_LOG(
		"header length's read failed. arrived:%d  expected:%zd\n",
		req_hdr->req_hdr_len, sizeof(struct xio_tcp_req_hdr));
		return -1;
	}

	UNPACK_SVAL(tmp_req_hdr, req_hdr, tid);
	req_hdr->opcode		= tmp_req_hdr->opcode;

	UNPACK_SVAL(tmp_req_hdr, req_hdr, recv_num_sge);
	UNPACK_SVAL(tmp_req_hdr, req_hdr, read_num_sge);
	UNPACK_SVAL(tmp_req_hdr, req_hdr, write_num_sge);
	UNPACK_SVAL(tmp_req_hdr, req_hdr, ulp_hdr_len);
	UNPACK_SVAL(tmp_req_hdr, req_hdr, ulp_pad_len);

	/* remain_data_len not in use */
	UNPACK_LLVAL(tmp_req_hdr, req_hdr, ulp_imm_len);

	tmp_sge = (void *)((uint8_t *)tmp_req_hdr +
			sizeof(struct xio_tcp_req_hdr));

	/* params for SEND */
	for (i = 0;  i < req_hdr->recv_num_sge; i++) {
		UNPACK_LLVAL(tmp_sge, &tcp_task->req_recv_sge[i], addr);
		UNPACK_LVAL(tmp_sge, &tcp_task->req_recv_sge[i], length);
		UNPACK_LVAL(tmp_sge, &tcp_task->req_recv_sge[i], stag);
		tmp_sge++;
	}
	tcp_task->req_recv_num_sge	= i;

	/* params for RDMA_WRITE */
	for (i = 0;  i < req_hdr->read_num_sge; i++) {
		UNPACK_LLVAL(tmp_sge, &tcp_task->req_read_sge[i], addr);
		UNPACK_LVAL(tmp_sge, &tcp_task->req_read_sge[i], length);
		UNPACK_LVAL(tmp_sge, &tcp_task->req_read_sge[i], stag);
		tmp_sge++;
	}
	tcp_task->req_read_num_sge	= i;

	/* params for RDMA_READ */
	for (i = 0;  i < req_hdr->write_num_sge; i++) {
		UNPACK_LLVAL(tmp_sge, &tcp_task->req_write_sge[i], addr);
		UNPACK_LVAL(tmp_sge, &tcp_task->req_write_sge[i], length);
		UNPACK_LVAL(tmp_sge, &tcp_task->req_write_sge[i], stag);
		tmp_sge++;
	}
	tcp_task->req_write_num_sge	= i;

	hdr_len	= sizeof(struct xio_tcp_req_hdr);
	hdr_len += sizeof(struct xio_sge)*(req_hdr->recv_num_sge +
						   req_hdr->read_num_sge +
						   req_hdr->write_num_sge);

	xio_mbuf_inc(&task->mbuf, hdr_len);

	return 0;
}

/*---------------------------------------------------------------------------*/
/* xio_tcp_read_rsp_header						     */
/*---------------------------------------------------------------------------*/
static int xio_tcp_read_rsp_header(struct xio_tcp_transport *tcp_hndl,
				   struct xio_task *task,
				   struct xio_tcp_rsp_hdr *rsp_hdr)
{
	struct xio_tcp_rsp_hdr		*tmp_rsp_hdr;
	uint32_t			*wr_len;
	int				i;
	size_t				hdr_len;
	XIO_TO_TCP_TASK(task, tcp_task);

	/* point to transport header */
	xio_mbuf_set_trans_hdr(&task->mbuf);
	tmp_rsp_hdr = xio_mbuf_get_curr_ptr(&task->mbuf);

	rsp_hdr->version  = tmp_rsp_hdr->version;
	rsp_hdr->flags    = tmp_rsp_hdr->flags;
	UNPACK_SVAL(tmp_rsp_hdr, rsp_hdr, rsp_hdr_len);

	if (rsp_hdr->rsp_hdr_len != sizeof(struct xio_tcp_rsp_hdr)) {
		ERROR_LOG(
		"header length's read failed. arrived:%d expected:%zd\n",
		  rsp_hdr->rsp_hdr_len, sizeof(struct xio_tcp_rsp_hdr));
		return -1;
	}

	UNPACK_SVAL(tmp_rsp_hdr, rsp_hdr, tid);
	rsp_hdr->opcode = tmp_rsp_hdr->opcode;
	UNPACK_LVAL(tmp_rsp_hdr, rsp_hdr, status);
	UNPACK_SVAL(tmp_rsp_hdr, rsp_hdr, write_num_sge);
	UNPACK_SVAL(tmp_rsp_hdr, rsp_hdr, ulp_hdr_len);
	UNPACK_SVAL(tmp_rsp_hdr, rsp_hdr, ulp_pad_len);
	/* remain_data_len not in use */
	UNPACK_LLVAL(tmp_rsp_hdr, rsp_hdr, ulp_imm_len);

	if (rsp_hdr->write_num_sge) {
		wr_len = (void *)((uint8_t *)tmp_rsp_hdr +
				sizeof(struct xio_tcp_rsp_hdr));

		/* params for RDMA WRITE */
		for (i = 0;  i < rsp_hdr->write_num_sge; i++) {
			tcp_task->rsp_write_sge[i].length = ntohl(*wr_len);
			wr_len++;
		}
		tcp_task->rsp_write_num_sge = rsp_hdr->write_num_sge;
	}

	hdr_len	= sizeof(struct xio_tcp_rsp_hdr);
	hdr_len += sizeof(uint32_t)*rsp_hdr->write_num_sge;

	xio_mbuf_inc(&task->mbuf, hdr_len);

	return 0;
}

/*---------------------------------------------------------------------------*/
/* xio_tcp_notify_assign_in_buf					     */
/*---------------------------------------------------------------------------*/
static int xio_tcp_assign_in_buf(struct xio_tcp_transport *tcp_hndl,
				 struct xio_task *task, int *is_assigned)
{
	union xio_transport_event_data event_data = {
			.assign_in_buf.task	   = task,
			.assign_in_buf.is_assigned = 0
	};

	xio_transport_notify_observer(&tcp_hndl->base,
				      XIO_TRANSPORT_ASSIGN_IN_BUF,
				      &event_data);

	*is_assigned = event_data.assign_in_buf.is_assigned;
	return 0;
}

static int xio_tcp_recv_work(struct xio_tcp_transport *tcp_hndl,
			     struct xio_tcp_work_req *xio_recv,
			     int block)
{
	int			i, retval;
	int			recv_bytes = 0, tmp_bytes;

	while (xio_recv->tot_iov_byte_len) {
		retval = recvmsg(tcp_hndl->sock_fd, &xio_recv->msg, 0);
		if (retval > 0) {
			recv_bytes += retval;
			xio_recv->tot_iov_byte_len -= retval;

			if (xio_recv->tot_iov_byte_len == 0)
				break;

			tmp_bytes = 0;
			for (i = 0; i < xio_recv->msg.msg_iovlen; i++) {
				if (xio_recv->msg.msg_iov[i].iov_len +
						tmp_bytes < retval) {
					tmp_bytes +=
					xio_recv->msg.msg_iov[i].iov_len;
				} else {
					xio_recv->msg.msg_iov[i].iov_len -=
							(retval - tmp_bytes);
					xio_recv->msg.msg_iov[i].iov_base +=
							(retval - tmp_bytes);
					xio_recv->msg.msg_iov =
						&xio_recv->msg.msg_iov[i];
					xio_recv->msg.msg_iovlen -= i;
					break;
				}
			}
		} else if (retval == 0) {
			xio_set_error(ECONNABORTED); /*so errno is not EAGAIN*/
			DEBUG_LOG("tcp transport got EOF, tcp_hndl=%p\n",
				  tcp_hndl);
			return 0;
		} else {
			if (errno == EAGAIN) {
				if (!block) {
					xio_set_error(errno);
					return -1;
				}
			} else if (errno == ECONNRESET) {
				xio_set_error(errno);
				DEBUG_LOG("recvmsg failed. (errno=%d %s)\n",
					  errno, strerror(retval));
				return 0;
			} else {
				xio_set_error(errno);
				ERROR_LOG("recvmsg failed. (errno=%d %s)\n",
					  errno, strerror(retval));
				return -1;
			}
		}
	}

	return recv_bytes;
}

/*---------------------------------------------------------------------------*/
/* xio_tcp_rd_req_header						     */
/*---------------------------------------------------------------------------*/
static int xio_tcp_rd_req_header(struct xio_tcp_transport *tcp_hndl,
				 struct xio_task *task)
{
	XIO_TO_TCP_TASK(task, tcp_task);
	int			i, retval;
	int			user_assign_flag = 0;
	size_t			rlen = 0;

	/* responder side got request for rdma read */

	/* need for buffer to do rdma read. there are two options:	   */
	/* option 1: user provides call back that fills application memory */
	/* option 2: use internal buffer pool				   */

	/* hint the upper layer of sizes */
	for (i = 0;  i < tcp_task->req_write_num_sge; i++) {
		task->imsg.in.pdata_iov[i].iov_base  = NULL;
		task->imsg.in.pdata_iov[i].iov_len  =
					tcp_task->req_write_sge[i].length;
		rlen += tcp_task->req_write_sge[i].length;
		tcp_task->read_sge[i].cache = NULL;
	}
	task->imsg.in.data_iovlen = tcp_task->req_write_num_sge;

	for (i = 0;  i < tcp_task->req_read_num_sge; i++) {
		task->imsg.out.pdata_iov[i].iov_base  = NULL;
		task->imsg.out.pdata_iov[i].iov_len  =
					tcp_task->req_read_sge[i].length;
		tcp_task->write_sge[i].cache = NULL;
	}
	for (i = 0;  i < tcp_task->req_recv_num_sge; i++) {
		task->imsg.out.pdata_iov[i].iov_base  = NULL;
		task->imsg.out.pdata_iov[i].iov_len  =
					tcp_task->req_recv_sge[i].length;
		task->imsg.out.pdata_iov[i].mr  = NULL;
	}
	if (tcp_task->req_read_num_sge)
		task->imsg.out.data_iovlen = tcp_task->req_read_num_sge;
	else if (tcp_task->req_recv_num_sge)
		task->imsg.out.data_iovlen = tcp_task->req_recv_num_sge;
	else
		task->imsg.out.data_iovlen = 0;

	xio_tcp_assign_in_buf(tcp_hndl, task, &user_assign_flag);

	if (user_assign_flag) {
		/* if user does not have buffers ignore */
		if (task->imsg.in.data_iovlen == 0) {
			WARN_LOG("application has not provided buffers\n");
			WARN_LOG("tcp read is ignored\n");
			task->imsg.status = XIO_E_NO_USER_BUFS;
			return -1;
		}

		if (tcp_task->req_write_num_sge > task->imsg.in.data_iovlen) {
			WARN_LOG("app has not provided enough buffers\n");
			WARN_LOG("provided=%d, required=%d\n",
				 task->imsg.in.data_iovlen,
				 tcp_task->req_write_num_sge);
			WARN_LOG("tcp read is ignored\n");
			task->imsg.status = XIO_E_USER_BUF_OVERFLOW;
			return -1;
		}

		/*
		for (i = 0;  i < task->imsg.in.data_iovlen; i++) {
			if (task->imsg.in.pdata_iov[i].mr == NULL) {
				ERROR_LOG("application has not provided mr\n");
				ERROR_LOG("tcp read is ignored\n");
				task->imsg.status = EINVAL;
				return -1;
			}
			llen += task->imsg.in.pdata_iov[i].iov_len;
		}
		if (rlen  > llen) {
			ERROR_LOG("application provided too small iovec\n");
			ERROR_LOG("remote peer want to write %zd bytes while" \
				  "local peer provided buffer size %zd bytes\n",
				  rlen, llen);
			ERROR_LOG("tcp read is ignored\n");
			task->imsg.status = EINVAL;
			return -1;
		}
		*/

		/* ORK todo force user to provide same iovec,
		 or receive data in a different segmentation
		 as long as llen > rlen?? */
		for (i = 0;  i < tcp_task->req_write_num_sge; i++) {
			if (task->imsg.in.pdata_iov[i].iov_len <
			    tcp_task->req_write_sge[i].length) {
				ERROR_LOG("app provided too small iovec\n");
				ERROR_LOG("iovec #%d: len=%d, required=%d\n",
					  i,
					  task->imsg.in.data_iov[i].iov_len,
					  tcp_task->req_write_sge[i].length);
				ERROR_LOG("tcp read is ignored\n");
				task->imsg.status = XIO_E_USER_BUF_OVERFLOW;
				return -1;
			}
		}
	} else {
		if (tcp_hndl->tcp_mempool == NULL) {
				ERROR_LOG("message /read/write failed - " \
					  "library's memory pool disabled\n");
				task->imsg.status = XIO_E_NO_BUFS;
				goto cleanup;
		}

		for (i = 0;  i < tcp_task->req_write_num_sge; i++) {
			retval = xio_mempool_alloc(
					tcp_hndl->tcp_mempool,
					tcp_task->req_write_sge[i].length,
					&tcp_task->read_sge[i]);

			if (retval) {
				tcp_task->read_num_sge = i;
				ERROR_LOG("mempool is empty for %zd bytes\n",
					  tcp_task->read_sge[i].length);

				task->imsg.status = ENOMEM;
				goto cleanup;
			}
			task->imsg.in.pdata_iov[i].iov_base =
					tcp_task->read_sge[i].addr;
			task->imsg.in.pdata_iov[i].iov_len  =
					tcp_task->read_sge[i].length;
			task->imsg.in.pdata_iov[i].mr =
					tcp_task->read_sge[i].mr;
		}
		task->imsg.in.data_iovlen = tcp_task->req_write_num_sge;
		tcp_task->read_num_sge = tcp_task->req_write_num_sge;
	}

	for (i = 0;  i < tcp_task->req_write_num_sge; i++) {
		tcp_task->rxd.msg_iov[i].iov_base =
				task->imsg.in.pdata_iov[i].iov_base;
		tcp_task->rxd.msg_iov[i].iov_len =
				tcp_task->req_write_sge[i].length;
	}
	tcp_task->rxd.msg_len = tcp_task->req_write_num_sge;

	/* prepare the in side of the message */
	tcp_task->rxd.tot_iov_byte_len = rlen;
	tcp_task->rxd.msg.msg_iov = tcp_task->rxd.msg_iov;
	tcp_task->rxd.msg.msg_iovlen = tcp_task->rxd.msg_len;

	return 0;
cleanup:
	for (i = 0; i < tcp_task->read_num_sge; i++)
		xio_mempool_free(&tcp_task->read_sge[i]);

	tcp_task->read_num_sge = 0;
	return -1;
}

/*---------------------------------------------------------------------------*/
/* xio_tcp_rd_req_data							     */
/*---------------------------------------------------------------------------*/
static int xio_tcp_rd_req_data(struct xio_tcp_transport *tcp_hndl,
			       struct xio_task *task)
{
	XIO_TO_TCP_TASK(task, tcp_task);
	int			i, retval;

	retval = xio_tcp_recv_work(tcp_hndl, &tcp_task->rxd, 0);
	if (retval == 0)
		goto cleanup;
	else if (retval < 0)
		return retval;

	return retval;
cleanup:
	for (i = 0; i < tcp_task->read_num_sge; i++)
		xio_mempool_free(&tcp_task->read_sge[i]);

	tcp_task->read_num_sge = 0;
	return retval;
}

/*---------------------------------------------------------------------------*/
/* xio_tcp_on_recv_req_header						     */
/*---------------------------------------------------------------------------*/
static int xio_tcp_on_recv_req_header(struct xio_tcp_transport *tcp_hndl,
				      struct xio_task *task)
{
	int			retval = 0;
	XIO_TO_TCP_TASK(task, tcp_task);
	struct xio_tcp_req_hdr	req_hdr;
	struct xio_msg		*imsg;
	void			*ulp_hdr;
	int			i;

	/* read header */
	retval = xio_tcp_read_req_header(tcp_hndl, task, &req_hdr);
	if (retval != 0) {
		xio_set_error(XIO_E_MSG_INVALID);
		goto cleanup;
	}

	/* save originator identifier */
	task->rtid		= req_hdr.tid;
	task->imsg_flags	= req_hdr.flags;
	task->imsg.more_in_batch = tcp_task->more_in_batch;

	imsg = &task->imsg;
	ulp_hdr = xio_mbuf_get_curr_ptr(&task->mbuf);

	imsg->type = task->tlv_type;
	imsg->in.header.iov_len	= req_hdr.ulp_hdr_len;

	if (req_hdr.ulp_hdr_len)
		imsg->in.header.iov_base	= ulp_hdr;
	else
		imsg->in.header.iov_base	= NULL;

	/* hint upper layer about expected response */
	for (i = 0;  i < tcp_task->req_read_num_sge; i++) {
		imsg->out.pdata_iov[i].iov_base  = NULL;
		imsg->out.pdata_iov[i].iov_len  =
				tcp_task->req_read_sge[i].length;
		imsg->out.pdata_iov[i].mr  = NULL;
	}
	for (i = 0;  i < tcp_task->req_recv_num_sge; i++) {
		imsg->out.pdata_iov[i].iov_base  = NULL;
		imsg->out.pdata_iov[i].iov_len  =
				tcp_task->req_recv_sge[i].length;
		imsg->out.pdata_iov[i].mr  = NULL;
	}

	if (tcp_task->req_read_num_sge)
		imsg->out.data_iovlen = tcp_task->req_read_num_sge;
	else if (tcp_task->req_recv_num_sge)
		imsg->out.data_iovlen = tcp_task->req_recv_num_sge;
	else
		imsg->out.data_iovlen = 0;

	tcp_task->tcp_op = req_hdr.opcode;

	switch (req_hdr.opcode) {
	case XIO_TCP_SEND:
		if (req_hdr.ulp_imm_len) {
			imsg->in.pdata_iov[0].iov_len	= req_hdr.ulp_imm_len;
			imsg->in.pdata_iov[0].iov_base	= ulp_hdr +
					imsg->in.header.iov_len +
					req_hdr.ulp_pad_len;
			imsg->in.data_iovlen		= 1;
		} else {
			/* no data at all */
			imsg->in.pdata_iov[0].iov_base	= NULL;
			imsg->in.data_iovlen		= 0;
		}
		break;
	case XIO_TCP_READ:
		/* handle RDMA READ equivalent. */
		TRACE_LOG("tcp read header\n");
		retval = xio_tcp_rd_req_header(tcp_hndl, task);
		if (retval) {
			ERROR_LOG("tcp read header failed\n");
			goto cleanup;
		}
		break;
	default:
		ERROR_LOG("unexpected opcode\n");
		xio_set_error(XIO_E_MSG_INVALID);
		imsg->status = XIO_E_MSG_INVALID;
		break;
	};

	return 0;

cleanup:
	retval = xio_errno();
	ERROR_LOG("xio_tcp_on_recv_req failed. (errno=%d %s)\n", retval,
		  xio_strerror(retval));
	xio_transport_notify_observer_error(&tcp_hndl->base, retval);

	return -1;
}

/*---------------------------------------------------------------------------*/
/* xio_tcp_on_recv_req_data						     */
/*---------------------------------------------------------------------------*/
static int xio_tcp_on_recv_req_data(struct xio_tcp_transport *tcp_hndl,
				    struct xio_task *task)
{
	int			retval = 0;
	XIO_TO_TCP_TASK(task, tcp_task);
	union xio_transport_event_data event_data;

	switch (tcp_task->tcp_op) {
	case XIO_TCP_SEND:
		break;
	case XIO_TCP_READ:
		/* handle RDMA READ equivalent. */
		TRACE_LOG("tcp read data\n");
		retval = xio_tcp_rd_req_data(tcp_hndl, task);
		if (retval < 0) {
			if (errno == EAGAIN)
				return -1;
			ERROR_LOG("tcp read data failed\n");
			goto cleanup;
		} else if (retval == 0) {
			ERROR_LOG("tcp transport got EOF, tcp_hndl=%p\n",
				  tcp_hndl);
			on_sock_disconnected(tcp_hndl, 1);
			goto cleanup;
		}
		break;
	default:
		ERROR_LOG("unexpected opcode\n");
		goto cleanup;
		break;
	};


	/* fill notification event */
	event_data.msg.op	= XIO_WC_OP_RECV;
	event_data.msg.task	= task;

	list_move_tail(&task->tasks_list_entry, &tcp_hndl->io_list);

	xio_transport_notify_observer(&tcp_hndl->base,
				      XIO_TRANSPORT_NEW_MESSAGE, &event_data);

	return 0;

cleanup:
	retval = xio_errno();
	ERROR_LOG("xio_tcp_on_recv_req failed. (errno=%d %s)\n", retval,
		  xio_strerror(retval));
	xio_transport_notify_observer_error(&tcp_hndl->base, retval);

	return -1;
}

/*---------------------------------------------------------------------------*/
/* xio_tcp_on_recv_rsp_header						     */
/*---------------------------------------------------------------------------*/
static int xio_tcp_on_recv_rsp_header(struct xio_tcp_transport *tcp_hndl,
				      struct xio_task *task)
{
	int			retval = 0;
	struct xio_tcp_rsp_hdr	rsp_hdr;
	struct xio_msg		*imsg;
	struct xio_msg		*omsg;
	void			*ulp_hdr;
	XIO_TO_TCP_TASK(task, tcp_task);
	XIO_TO_TCP_TASK(task, tcp_sender_task);
	int			i;

	/* read the response header */
	retval = xio_tcp_read_rsp_header(tcp_hndl, task, &rsp_hdr);
	if (retval != 0) {
		xio_set_error(XIO_E_MSG_INVALID);
		goto cleanup;
	}

	task->imsg.more_in_batch = tcp_task->more_in_batch;

	/* find the sender task */
	task->sender_task =
		xio_tcp_primary_task_lookup(tcp_hndl, rsp_hdr.tid);

	tcp_sender_task = task->sender_task->dd_data;

	/* mark the sender task as arrived */
	task->sender_task->state = XIO_TASK_STATE_RESPONSE_RECV;

	omsg = task->sender_task->omsg;
	imsg = &task->imsg;

	ulp_hdr = xio_mbuf_get_curr_ptr(&task->mbuf);
	/* msg from received message */
	if (rsp_hdr.ulp_hdr_len) {
		imsg->in.header.iov_base	= ulp_hdr;
		imsg->in.header.iov_len		= rsp_hdr.ulp_hdr_len;
	} else {
		imsg->in.header.iov_base	= NULL;
		imsg->in.header.iov_len		= 0;
	}
	omsg->status = rsp_hdr.status;

	/* handle the headers */
	if (omsg->in.header.iov_base) {
		/* copy header to user buffers */
		size_t hdr_len = 0;
		if (imsg->in.header.iov_len > omsg->in.header.iov_len)  {
			hdr_len = omsg->in.header.iov_len;
			omsg->status = XIO_E_MSG_SIZE;
		} else {
			hdr_len = imsg->in.header.iov_len;
			omsg->status = XIO_E_SUCCESS;
		}
		if (hdr_len)
			memcpy(omsg->in.header.iov_base,
			       imsg->in.header.iov_base,
			       hdr_len);
		else
			*((char *)omsg->in.header.iov_base) = 0;

		omsg->in.header.iov_len = hdr_len;
	} else {
		/* no copy - just pointers */
		memclonev(&omsg->in.header, 1, &imsg->in.header, 1);
	}

	tcp_task->tcp_op = rsp_hdr.opcode;

	switch (rsp_hdr.opcode) {
	case XIO_TCP_SEND:
		/* if data arrived, set the pointers */
		if (rsp_hdr.ulp_imm_len) {
			imsg->in.pdata_iov[0].iov_base	= ulp_hdr +
					imsg->in.header.iov_len +
					rsp_hdr.ulp_pad_len;
			imsg->in.pdata_iov[0].iov_len	= rsp_hdr.ulp_imm_len;
			imsg->in.data_iovlen		= 1;
		} else {
			imsg->in.pdata_iov[0].iov_base	= NULL;
			imsg->in.pdata_iov[0].iov_len	= 0;
			imsg->in.data_iovlen		= 0;
		}
		if (omsg->in.data_iovlen) {
			/* deep copy */
			if (imsg->in.data_iovlen) {
				size_t idata_len  = xio_iovex_length(
						imsg->in.pdata_iov,
						imsg->in.data_iovlen);
				size_t odata_len  = xio_iovex_length(
						omsg->in.pdata_iov,
						omsg->in.data_iovlen);

				if (idata_len > odata_len) {
					omsg->status = XIO_E_MSG_SIZE;
					goto partial_msg;
				} else {
					omsg->status = XIO_E_SUCCESS;
				}
				if (omsg->in.pdata_iov[0].iov_base)  {
					/* user provided buffer so do copy */
					omsg->in.data_iovlen = memcpyv_ex(
							omsg->in.pdata_iov,
							omsg->in.data_iovlen,
							imsg->in.pdata_iov,
							imsg->in.data_iovlen);
				} else {
					/* use provided only length - set user
					 * pointers */
					omsg->in.data_iovlen =  memclonev_ex(
							omsg->in.pdata_iov,
							omsg->in.data_iovlen,
							imsg->in.pdata_iov,
							imsg->in.data_iovlen);
				}
			} else {
				omsg->in.data_iovlen = imsg->in.data_iovlen;
			}
		} else {
			omsg->in.data_iovlen =
					memclonev_ex(omsg->in.pdata_iov,
						     tcp_options.max_in_iovsz,
						     imsg->in.pdata_iov,
						     imsg->in.data_iovlen);
		}
		break;
	case XIO_TCP_WRITE:
		if (tcp_task->rsp_write_num_sge >
		    tcp_sender_task->read_num_sge) {
			ERROR_LOG("local in data_iovec is too small %d < %d\n",
				  tcp_sender_task->read_num_sge,
				  tcp_task->rsp_write_num_sge);
			goto partial_msg;
		}

		for (i = 0; i < tcp_task->rsp_write_num_sge; i++) {
			imsg->in.pdata_iov[i].iov_base	=
					tcp_sender_task->read_sge[i].addr;
			imsg->in.pdata_iov[i].iov_len	=
					tcp_task->rsp_write_sge[i].length;
			tcp_sender_task->rxd.msg_iov[i].iov_base =
					tcp_sender_task->read_sge[i].addr;
			tcp_sender_task->rxd.msg_iov[i].iov_len =
					tcp_task->rsp_write_sge[i].length;
		}

		imsg->in.data_iovlen = tcp_task->rsp_write_num_sge;

		tcp_sender_task->rxd.msg_len =
				tcp_task->rsp_write_num_sge;
		tcp_sender_task->rxd.tot_iov_byte_len =
				rsp_hdr.ulp_imm_len;
		tcp_sender_task->rxd.msg.msg_iov =
				tcp_sender_task->rxd.msg_iov;
		tcp_sender_task->rxd.msg.msg_iovlen =
				tcp_sender_task->rxd.msg_len;
		break;
	default:
		ERROR_LOG("unexpected opcode %d\n", rsp_hdr.opcode);
		break;
	}

partial_msg:
	return 0;

cleanup:
	retval = xio_errno();
	ERROR_LOG("xio_tcp_on_recv_rsp failed. (errno=%d %s)\n",
		  retval, xio_strerror(retval));
	xio_transport_notify_observer_error(&tcp_hndl->base, retval);

	return -1;
}

/*---------------------------------------------------------------------------*/
/* xio_tcp_on_recv_rsp_data						     */
/*---------------------------------------------------------------------------*/
static int xio_tcp_on_recv_rsp_data(struct xio_tcp_transport *tcp_hndl,
				    struct xio_task *task)
{
	int			retval = 0;
	union xio_transport_event_data event_data;
	struct xio_msg		*imsg;
	struct xio_msg		*omsg;
	int			i;
	XIO_TO_TCP_TASK(task, tcp_task);
	XIO_TO_TCP_TASK(task, tcp_sender_task);

	switch (tcp_task->tcp_op) {
	case XIO_TCP_SEND:
		break;
	case XIO_TCP_WRITE:
		tcp_sender_task = task->sender_task->dd_data;
		omsg = task->sender_task->omsg;
		imsg = &task->imsg;

		retval = xio_tcp_recv_work(tcp_hndl, &tcp_sender_task->rxd, 0);
		if (retval == 0) {
			ERROR_LOG("tcp transport got EOF, tcp_hndl=%p\n",
				  tcp_hndl);
			on_sock_disconnected(tcp_hndl, 1);
			goto cleanup;
		} else if (retval < 0) {
			if (xio_errno() != EAGAIN)
				goto cleanup;
			return retval;
		}

		/* user provided mr */
		if (omsg->in.pdata_iov[0].mr)  {
			/* data was copied directly to user buffer */
			/* need to update the buffer length */
			omsg->in.data_iovlen = imsg->in.data_iovlen;
			for (i = 0; i < omsg->in.data_iovlen; i++) {
				omsg->in.pdata_iov[i].iov_len =
						imsg->in.pdata_iov[i].iov_len;
			}
		} else  {
			/* user provided buffer but not mr */
			/* deep copy */

			if (omsg->in.pdata_iov[0].iov_base)  {
				omsg->in.data_iovlen = memcpyv_ex(
						omsg->in.pdata_iov,
						omsg->in.data_iovlen,
						imsg->in.pdata_iov,
						imsg->in.data_iovlen);

				/* put buffers back to pool */
				for (i = 0; i < tcp_sender_task->read_num_sge;
						i++) {
					xio_mempool_free(&tcp_sender_task->
							 read_sge[i]);
					tcp_sender_task->read_sge[i].cache = 0;
				}
				tcp_sender_task->read_num_sge = 0;
			} else {
				/* use provided only length - set user
				 * pointers */
				omsg->in.data_iovlen = memclonev_ex(
						omsg->in.pdata_iov,
						omsg->in.data_iovlen,
						imsg->in.pdata_iov,
						imsg->in.data_iovlen);
			}
		}
		break;
	default:
		ERROR_LOG("unexpected opcode %d\n", tcp_task->tcp_op);
		break;
	}

	/* fill notification event */
	event_data.msg.op	= XIO_WC_OP_RECV;
	event_data.msg.task	= task;

	list_move_tail(&task->tasks_list_entry, &tcp_hndl->io_list);

	/* notify the upper layer of received message */
	xio_transport_notify_observer(&tcp_hndl->base,
				      XIO_TRANSPORT_NEW_MESSAGE,
				      &event_data);
	return 0;

cleanup:
	retval = xio_errno();
	ERROR_LOG("xio_tcp_on_recv_rsp failed. (errno=%d %s)\n",
		  retval, xio_strerror(retval));
	xio_transport_notify_observer_error(&tcp_hndl->base, retval);

	return -1;
}

/*---------------------------------------------------------------------------*/
/* xio_tcp_send							     */
/*---------------------------------------------------------------------------*/
int xio_tcp_send(struct xio_transport_base *transport,
		 struct xio_task *task)
{
	struct xio_tcp_transport *tcp_hndl =
		(struct xio_tcp_transport *)transport;
	int	retval = -1;

	switch (task->tlv_type) {
	case XIO_NEXUS_SETUP_REQ:
		retval = xio_tcp_send_setup_req(tcp_hndl, task);
		break;
	case XIO_NEXUS_SETUP_RSP:
		retval = xio_tcp_send_setup_rsp(tcp_hndl, task);
		break;
	default:
		if (IS_REQUEST(task->tlv_type))
			retval = xio_tcp_send_req(tcp_hndl, task);
		else if (IS_RESPONSE(task->tlv_type))
			retval = xio_tcp_send_rsp(tcp_hndl, task);
		else
			ERROR_LOG("unknown message type:0x%x\n",
				  task->tlv_type);
		break;
	}

	return retval;
}

/*---------------------------------------------------------------------------*/
/* xio_tcp_rx_handler							     */
/*---------------------------------------------------------------------------*/
int xio_tcp_rx_handler(struct xio_tcp_transport *tcp_hndl)
{
	int retval = 0;
	struct xio_tcp_task *tcp_task;
	struct xio_task *task, *task_next/*, *task1 = NULL, *task2*/;

	task = list_first_entry_or_null(&tcp_hndl->rx_list,
					struct xio_task,
					tasks_list_entry);
	if (task == NULL)
		return 0;

	/* prefetch next buffer */
	/*if (!list_empty(&task->tasks_list_entry)) {
		task1 = list_first_entry(&task->tasks_list_entry,
					 struct xio_task, tasks_list_entry);
		xio_prefetch(task1->mbuf.buf.head);
	}*/

	tcp_task = task->dd_data;

	switch (tcp_task->rxd.stage) {
	case XIO_TCP_RX_START:
		/* ORK todo find a better place to rearm rx_list?*/
		if (tcp_hndl->state == XIO_STATE_CONNECTED) {
			task_next = xio_tcp_primary_task_alloc(tcp_hndl);
			if (task_next == NULL) {
				ERROR_LOG("primary task pool is empty\n");
			} else {
				list_add_tail(&task_next->tasks_list_entry,
					      &tcp_hndl->rx_list);
			}
		}
		tcp_task->rxd.tot_iov_byte_len = sizeof(struct xio_tlv);
		tcp_task->rxd.msg.msg_iov = tcp_task->rxd.msg_iov;
		tcp_task->rxd.msg.msg_iovlen = 1;
		tcp_task->rxd.stage = XIO_TCP_RX_TLV;
		/*fallthrough*/
	case XIO_TCP_RX_TLV:
		retval = xio_tcp_recv_work(tcp_hndl, &tcp_task->rxd, 0);
		if (retval == 0) {
			DEBUG_LOG("tcp transport got EOF, tcp_hndl=%p\n",
				  tcp_hndl);
			on_sock_disconnected(tcp_hndl, 1);
			return -1;
		} else if (retval < 0) {
			return retval;
		}
		retval = xio_mbuf_read_first_tlv(&task->mbuf);
		tcp_task->rxd.msg.msg_iov[0].iov_base =
				tcp_task->rxd.msg_iov[1].iov_base;
		tcp_task->rxd.msg.msg_iov[0].iov_len = task->mbuf.tlv.len;
		tcp_task->rxd.tot_iov_byte_len = task->mbuf.tlv.len;
		tcp_task->rxd.stage = XIO_TCP_RX_HEADER;
		/*fallthrough*/
	case XIO_TCP_RX_HEADER:
		retval = xio_tcp_recv_work(tcp_hndl, &tcp_task->rxd, 0);
		if (retval == 0) {
			DEBUG_LOG("tcp transport got EOF, tcp_hndl=%p\n",
				  tcp_hndl);
			on_sock_disconnected(tcp_hndl, 1);
			return -1;
		} else if (retval < 0) {
			return retval;
		}
		task->tlv_type = xio_mbuf_tlv_type(&task->mbuf);
		/* call recv completion  */
		switch (task->tlv_type) {
		case XIO_NEXUS_SETUP_REQ:
		case XIO_NEXUS_SETUP_RSP:
			xio_tcp_on_setup_msg(tcp_hndl, task);
			return 1;
		/*
		case XIO_CANCEL_REQ:
			xio_rdma_on_recv_cancel_req(rdma_hndl, task);
			break;
		case XIO_CANCEL_RSP:
			xio_rdma_on_recv_cancel_rsp(rdma_hndl, task);
			break;
		*/
		default:
			if (IS_REQUEST(task->tlv_type))
				retval = xio_tcp_on_recv_req_header(tcp_hndl,
								    task);
			else if (IS_RESPONSE(task->tlv_type))
				retval = xio_tcp_on_recv_rsp_header(tcp_hndl,
								    task);
			else
				ERROR_LOG("unknown message type:0x%x\n",
					  task->tlv_type);
			if (retval < 0) {
				ERROR_LOG("error while reading header\n");
				return retval;
			}
		}
		tcp_task->rxd.stage = XIO_TCP_RX_IO_DATA;
		/*fallthrough*/
	case XIO_TCP_RX_IO_DATA:
		if (IS_REQUEST(task->tlv_type))
			retval = xio_tcp_on_recv_req_data(tcp_hndl, task);
		else if (IS_RESPONSE(task->tlv_type))
			retval = xio_tcp_on_recv_rsp_data(tcp_hndl, task);
		else
			ERROR_LOG("unknown message type:0x%x\n",
				  task->tlv_type);
		if (retval < 0)
			return retval;

		break;
	default:
		ERROR_LOG("unknown stage type:%d\n", tcp_task->rxd.stage);
		break;
	}

	if (tcp_hndl->tx_ready_tasks_num) {
		retval = xio_tcp_xmit(tcp_hndl);
		if (retval < 0)
			return 1;
	}

	/* prefetch next buffer */
	/*if (task1 && !list_empty(&task1->tasks_list_entry)) {
		task2 = list_first_entry(&task1->tasks_list_entry,
					 struct xio_task,  tasks_list_entry);
		xio_prefetch(task2->mbuf.buf.head);
	}*/

	return 1;
}

/*---------------------------------------------------------------------------*/
/* xio_tcp_poll								     */
/*---------------------------------------------------------------------------*/
int xio_tcp_poll(struct xio_transport_base *transport,
		 long min_nr, long max_nr,
		 struct timespec *ts_timeout)
{
	struct xio_tcp_transport	*tcp_hndl;
	int				nr_comp = 0, recv_counter;
	cycles_t			timeout = -1;
	cycles_t			start_time = get_cycles();

	if (min_nr > max_nr)
		return -1;

	if (ts_timeout)
		timeout = timespec_to_usecs(ts_timeout)*g_mhz;

	tcp_hndl = (struct xio_tcp_transport *)transport;

	if (tcp_hndl->state != XIO_STATE_CONNECTED) {
		ERROR_LOG("tcp transport is not connected, state=%d\n",
			  tcp_hndl->state);
		return -1;
	}

	while (1) {
		/* ORK todo blocking recv with timeout?*/
		recv_counter = xio_tcp_rx_handler(tcp_hndl);
		if (recv_counter < 0 && xio_errno() != EAGAIN)
			break;

		nr_comp += recv_counter;
		max_nr -= recv_counter;
		if (nr_comp >= min_nr || max_nr == 0)
			break;
		if ((get_cycles() - start_time) >= timeout)
			break;
	}

	return nr_comp;
}
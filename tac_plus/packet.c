/*
   Copyright (C) 1999-2016 Marc Huber (Marc.Huber@web.de)
   All rights reserved.

   Redistribution and use in source and binary  forms,  with or without
   modification, are permitted provided  that  the following conditions
   are met:

   1. Redistributions of source code  must  retain  the above copyright
      notice, this list of conditions and the following disclaimer.

   2. Redistributions in binary form must reproduce the above copyright
      notice, this list of conditions  and  the following disclaimer in
      the  documentation  and/or  other  materials  provided  with  the
      distribution.

   3. The end-user documentation  included with the redistribution,  if
      any, must include the following acknowledgment:

          This product includes software developed by Marc Huber
	  (Marc.Huber@web.de).

      Alternately,  this  acknowledgment  may  appear  in  the software
      itself, if and wherever such third-party acknowledgments normally
      appear.

   THIS SOFTWARE IS  PROVIDED  ``AS IS''  AND  ANY EXPRESSED OR IMPLIED
   WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF
   MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED.
   IN NO EVENT SHALL  ITS  AUTHOR  BE  LIABLE FOR ANY DIRECT, INDIRECT,
   INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING,
   BUT NOT LIMITED  TO,  PROCUREMENT OF  SUBSTITUTE  GOODS OR SERVICES;
   LOSS OF USE,  DATA,  OR PROFITS;  OR  BUSINESS INTERRUPTION) HOWEVER
   CAUSED AND ON ANY THEORY OF LIABILITY,  WHETHER IN CONTRACT,  STRICT
   LIABILITY,  OR TORT  (INCLUDING NEGLIGENCE OR OTHERWISE)  ARISING IN
   ANY WAY OUT OF THE  USE  OF  THIS  SOFTWARE,  EVEN IF ADVISED OF THE
   POSSIBILITY OF SUCH DAMAGE.
 */

/* 
   Copyright (c) 1995-1998 by Cisco systems, Inc.

   Permission to use, copy, modify, and distribute this software for
   any purpose and without fee is hereby granted, provided that this
   copyright and permission notice appear on all copies of the
   software and supporting documentation, the name of Cisco Systems,
   Inc. not be used in advertising or publicity pertaining to
   distribution of the program without specific prior permission, and
   notice be given in supporting documentation that modification,
   copying and distribution is by permission of Cisco Systems, Inc.

   Cisco Systems, Inc. makes no representations about the suitability
   of this software for any purpose.  THIS SOFTWARE IS PROVIDED ``AS
   IS'' AND WITHOUT ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING,
   WITHOUT LIMITATION, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND
   FITNESS FOR A PARTICULAR PURPOSE.
*/

#include "headers.h"
#include "misc/mymd5.h"

static const char rcsid[] __attribute__((used)) = "$Id$";

static void write_packet(struct context *, tac_pak *);
static tac_session *new_session(struct context *, tac_pak_hdr *);

static void md5_xor(tac_pak_hdr * hdr, char *key, int keylen)
{
    if (key && *key) {
	u_char *data = tac_payload(hdr, u_char *);
	int data_len = ntohl(hdr->datalength), h = 0;
	u_char hash[MD5_LEN][2];

	for (int i = 0; i < data_len; i += 16) {
	    int min = minimum(data_len - i, 16);
	    struct iovec iov[5] = {
		{.iov_base = (u_char *) & hdr->session_id,.iov_len = sizeof(hdr->session_id) },
		{.iov_base = key,.iov_len = keylen },
		{.iov_base = (u_char *) & hdr->version,.iov_len = sizeof(hdr->version) },
		{.iov_base = (u_char *) & hdr->seq_no,.iov_len = sizeof(hdr->seq_no) },
		{.iov_base = (u_char *) hash[h ^ 1],.iov_len = MD5_LEN }
	    };
	    md5v(hash[h], MD5_LEN, iov, i ? 5 : 4);

	    for (int j = 0; j < min; j++)
		data[i + j] ^= hash[h][j];
	    h ^= 1;
	}
	hdr->flags ^= TAC_PLUS_UNENCRYPTED_FLAG;
    }
}

static tac_pak *new_pak(tac_session * session, u_char type, int len)
{
    tac_pak *pak = mem_alloc(session->ctx->mem, sizeof(struct tac_pak) + len);
    pak->length = TAC_PLUS_HDR_SIZE + len;
    pak->hdr.type = type;
    pak->hdr.flags = TAC_PLUS_UNENCRYPTED_FLAG;
    pak->hdr.seq_no = ++session->seq_no;
    pak->hdr.version = session->version;
    pak->hdr.session_id = session->session_id;
    pak->hdr.datalength = htonl(len);
    return pak;
}

/* send an accounting response packet */
void send_acct_reply(tac_session * session, u_char status, char *msg, char *data)
{
    int msg_len = msg ? (int) strlen(msg) : 0;
    int data_len = data ? (int) strlen(data) : 0;
    int len = TAC_ACCT_REPLY_FIXED_FIELDS_SIZE + msg_len + data_len;

    tac_pak *pak = new_pak(session, TAC_PLUS_ACCT, len);

    struct acct_reply *reply = tac_payload(&pak->hdr, struct acct_reply *);
    reply->status = status;
    reply->msg_len = htons((u_short) msg_len);
    reply->data_len = htons((u_short) data_len);

    u_char *p = (u_char *) reply + TAC_ACCT_REPLY_FIXED_FIELDS_SIZE;
    memcpy(p, msg, msg_len);
    p += msg_len;
    memcpy(p, data, data_len);

    write_packet(session->ctx, pak);
    cleanup_session(session);
}

/* send an authorization reply packet */
void send_author_reply(tac_session * session, u_char status, char *msg, char *data, int arg_cnt, char **args)
{
    int msg_len = msg ? (int) strlen(msg) : 0;
    int data_len = data ? (int) strlen(data) : 0;
    size_t j = arg_cnt * sizeof(int);
    int *arglen = alloca(j);
    int user_msg_len = session->user_msg ? (int) strlen(session->user_msg) : 0;

    if ((user_msg_len + msg_len) & ~0xffff)
	user_msg_len = 0;

    msg_len += user_msg_len;

    msg_len = minimum(msg_len, 0xffff);
    data_len = minimum(data_len, 0xffff);

    int len = TAC_AUTHOR_REPLY_FIXED_FIELDS_SIZE + msg_len + data_len;

    for (int i = 0; i < arg_cnt; i++) {
	arglen[i] = (int) strlen(args[i]);
	len += arglen[i] + 1;
    }

    tac_pak *pak = new_pak(session, TAC_PLUS_AUTHOR, len);

    struct author_reply *reply = tac_payload(&pak->hdr, struct author_reply *);
    reply->status = status;
    reply->msg_len = htons((u_short) msg_len);
    reply->data_len = htons((u_short) data_len);
    reply->arg_cnt = arg_cnt;

    u_char *p = (u_char *) reply + TAC_AUTHOR_REPLY_FIXED_FIELDS_SIZE;

    /* place arg sizes into packet  */
    for (int i = 0; i < arg_cnt; i++)
	*p++ = arglen[i];

    if (user_msg_len) {
	memcpy(p, session->user_msg, user_msg_len);
	p += user_msg_len;
    }
    memcpy(p, msg, msg_len - user_msg_len);

    p += msg_len - user_msg_len;
    memcpy(p, data, data_len);
    p += data_len;

    /* copy arg bodies into packet */
    for (int i = 0; i < arg_cnt; i++) {
	memcpy(p, args[i], arglen[i]);
	p += arglen[i];
    }

    write_packet(session->ctx, pak);

    cleanup_session(session);
}

/* Send an authentication reply packet indicating an error has occurred. */

void send_authen_error(tac_session * session, char *fmt, ...)
{
    int nlen, len = 1024;
    char *msg = alloca(len);
    va_list ap;

    va_start(ap, fmt);
    nlen = vsnprintf(msg, len, fmt, ap);
    va_end(ap);
    if (len <= nlen) {
	nlen++;
	msg = alloca(nlen);
	va_start(ap, fmt);
	vsnprintf(msg, nlen, fmt, ap);
	va_end(ap);
    }

    report(session, LOG_ERR, ~0, "%s %s: %s", session->ctx->nas_address_ascii, session->nas_port, msg);
    send_authen_reply(session, TAC_PLUS_AUTHEN_STATUS_ERROR, msg, 0, NULL, 0, 0);
}

static void delay_packet(struct context *, tac_pak *, int);

/* create and send an authentication reply packet from tacacs+ to a NAS */

void send_authen_reply(tac_session * session, int status, char *msg, int msg_len, u_char * data, int data_len, u_char flags)
{
    int delay = 0;
    int user_msg_len = session->user_msg ? (int) strlen(session->user_msg) : 0;

    if (data && !data_len)
	data_len = (int) strlen((char *) data);
    if (msg && !msg_len)
	msg_len = (int) strlen(msg);

    if ((user_msg_len + msg_len) & ~0xffff)
	user_msg_len = 0;

    msg_len += user_msg_len;
    msg_len = minimum(msg_len, 0xffff);
    data_len = minimum(data_len, 0xffff);

    int len = TAC_AUTHEN_REPLY_FIXED_FIELDS_SIZE + msg_len + data_len;
    tac_pak *pak = new_pak(session, TAC_PLUS_AUTHEN, len);

    struct authen_reply *reply = tac_payload(&pak->hdr, struct authen_reply *);
    reply->status = status;
    reply->msg_len = htons((u_short) msg_len);
    reply->data_len = htons((u_short) data_len);
    reply->flags = flags;

    u_char *p = (u_char *) reply + TAC_AUTHEN_REPLY_FIXED_FIELDS_SIZE;

    if (user_msg_len) {
	memcpy(p, session->user_msg, user_msg_len);
	p += user_msg_len;
    }
    memcpy(p, msg, msg_len - user_msg_len);
    p += msg_len - user_msg_len;
    mem_free(session->mem, &session->user_msg);
    memcpy(p, data, data_len);

    if (session->password_bad && !session->password_bad_again) {
	delay = session->ctx->authfail_delay;

	if (delay > 0 && session->nac_address_valid) {
	    tac_host *arr[129];
	    int arr_min = 0, arr_max = 0, i;

	    memset(arr, 0, sizeof(arr));

	    if (radix_lookup(session->ctx->nac_realm->hosttree, &session->nac_address, (void *) arr)) {
		for (arr_max = 0; arr_max < 129 && arr[arr_max]; arr_max++);
		arr_max--;

		for (i = arr_max; i > -1 && !arr[i]->orphan; i--);
		arr_min = i;
		for (i = arr_max; i > arr_min; i--)
		    if (arr[i]->authfail_delay > -1) {
			if (arr[i]->authfail_delay < delay)
			    delay = arr[i]->authfail_delay;
			break;
		    }
	    }
	}
    }

    if (delay > 0)
	delay_packet(session->ctx, pak, delay);
    else
	write_packet(session->ctx, pak);

    switch (status) {
    case TAC_PLUS_AUTHEN_STATUS_FAIL:
    case TAC_PLUS_AUTHEN_STATUS_PASS:
    case TAC_PLUS_AUTHEN_STATUS_ERROR:
    case TAC_PLUS_AUTHEN_STATUS_FOLLOW:
	cleanup_session(session);
    }
}

static void write_delayed_packet(struct context *ctx, int cur __attribute__((unused)))
{
    while (ctx->delayed && ctx->delayed->delay_until <= io_now.tv_sec) {
	tac_pak *p = ctx->delayed->next;
	ctx->delayed->next = NULL;
	write_packet(ctx, ctx->delayed);
	ctx->delayed = p;
    }
    io_sched_del(ctx->io, ctx, (void *) write_delayed_packet);
    if (ctx->delayed)
	io_sched_add(ctx->io, ctx, (void *) write_delayed_packet, ctx->delayed->delay_until - io_now.tv_sec, 0);
}

static void delay_packet(struct context *ctx, tac_pak *p, int delay)
{

    p->delay_until = io_now.tv_sec + delay;

    tac_pak **pp;
    for (pp = &ctx->delayed; *pp && (*pp)->delay_until < p->delay_until; pp = &(*pp)->next);

    p->next = *pp;
    *pp = p;

    if (ctx->delayed == p)
	io_sched_add(ctx->io, ctx, (void *) write_delayed_packet, delay, 0);
}

/* write a packet to the wire, encrypting it */
static void write_packet(struct context *ctx, tac_pak * p)
{
    p->hdr.flags |= ctx->flags;

    if ((common_data.debug | ctx->debug) & DEBUG_PACKET_FLAG) {
	tac_session dummy_session = {
		.session_id = p->hdr.session_id,
		.ctx = ctx,
		.debug = ctx->debug,
	};
	report(&dummy_session, LOG_DEBUG, DEBUG_PACKET_FLAG, "Writing %s size=%d", summarise_outgoing_packet_type(&p->hdr), (int) p->length);
	dump_tacacs_pak(&dummy_session, &p->hdr);
    }

    /* encrypt the data portion */
    md5_xor(&p->hdr, ctx->key->key, ctx->key->len);

    tac_pak **pp;
    for (pp = &ctx->out; *pp; pp = &(*pp)->next);
    *pp = p;

    io_set_o(ctx->io, ctx->sock);
}

static int authen_pak_looks_bogus(tac_pak_hdr * hdr)
{
    struct authen_start *start = tac_payload(hdr, struct authen_start *);
    struct authen_cont *cont = tac_payload(hdr, struct authen_cont *);
    int datalength = ntohl(hdr->datalength);

    if ((hdr->seq_no == 1 && (datalength != TAC_AUTHEN_START_FIXED_FIELDS_SIZE + start->user_len + start->port_len + start->rem_addr_len + start->data_len))
	|| (hdr->seq_no > 2 && datalength != TAC_AUTHEN_CONT_FIXED_FIELDS_SIZE + ntohs(cont->user_msg_len) + ntohs(cont->user_data_len))) {
	return -1;
    }
    return 0;
}

static int author_pak_looks_bogus(tac_pak_hdr * hdr)
{
    struct author *pak = tac_payload(hdr, struct author *);
    u_char *p = (u_char *) pak + TAC_AUTHOR_REQ_FIXED_FIELDS_SIZE;
    u_int datalength = ntohl(hdr->datalength);
    u_int len = TAC_AUTHOR_REQ_FIXED_FIELDS_SIZE + pak->user_len + pak->port_len + pak->rem_addr_len + pak->arg_cnt;

    int i;
    for (i = 0; i < (int) pak->arg_cnt; i++)
	len += p[i];

    if (i != (int) pak->arg_cnt || len != datalength)
	return -1;
    return 0;
}

static int accounting_pak_looks_bogus(tac_pak_hdr * hdr)
{
    struct acct *pak = tac_payload(hdr, struct acct *);
    u_char *p = (u_char *) pak + TAC_ACCT_REQ_FIXED_FIELDS_SIZE;
    u_int datalength = ntohl(hdr->datalength);
    u_int len = TAC_ACCT_REQ_FIXED_FIELDS_SIZE + pak->user_len + pak->port_len + pak->rem_addr_len + pak->arg_cnt;

    int i;
    for (i = 0; i < (int) pak->arg_cnt && len < datalength; i++)
	len += p[i];

    return (i != (int) pak->arg_cnt || len != datalength);
}


static __inline__ tac_session *RB_lookup_session(rb_tree_t * rbt, int session_id)
{
    tac_session s = {.session_id = session_id };
    return RB_lookup(rbt, &s);
}

void tac_read(struct context *ctx, int cur)
{
    ssize_t len;
    int min_len = 1;

    ctx->last_io = io_now.tv_sec;

    if (ctx->hdroff != TAC_PLUS_HDR_SIZE) {
	len = read(cur, ((u_char *) & ctx->hdr) + ctx->hdroff, TAC_PLUS_HDR_SIZE - ctx->hdroff);
	if (len <= 0) {
	    cleanup(ctx, cur);
	    return;
	}
	ctx->hdroff += len;
	min_len = 0;
    }
    if (ctx->hdroff != TAC_PLUS_HDR_SIZE)
	return;
    if ((ctx->hdr.version & TAC_PLUS_MAJOR_VER_MASK) != TAC_PLUS_MAJOR_VER) {
	report(NULL, LOG_ERR, ~0, "%s: Illegal major version specified: " "found %d wanted %d", ctx->nas_address_ascii, ctx->hdr.version, TAC_PLUS_MAJOR_VER);
	cleanup(ctx, cur);
	return;
    }
    u_int data_len = ntohl(ctx->hdr.datalength);

    if (data_len & ~0xffffUL) {
	report(NULL, LOG_ERR, ~0, "%s: Illegal data size: %u", ctx->nas_address_ascii, data_len);
	cleanup(ctx, cur);
	return;
    }
    if (!ctx->in) {
	ctx->in = mem_alloc(ctx->mem, sizeof(tac_pak) + data_len);
	ctx->in->offset = TAC_PLUS_HDR_SIZE;
	ctx->in->length = TAC_PLUS_HDR_SIZE + data_len;
	memcpy(&ctx->in->hdr, &ctx->hdr, TAC_PLUS_HDR_SIZE);
    }
    len = read(cur, (u_char *) & ctx->in->hdr + ctx->in->offset, ctx->in->length - ctx->in->offset);
    if (len < min_len && min_len) {
	cleanup(ctx, cur);
	return;
    }
    if (len < 0)
	return;
    ctx->in->offset += len;
    if (ctx->in->offset != ctx->in->length)
	return;

    tac_session *session = RB_lookup_session(ctx->sessions, ctx->hdr.session_id);

    if (session) {
	session->seq_no++;
	if (!session->user_is_session_specific)
	    session->user = NULL;	/* may be outdated */
	if (session->seq_no != ctx->hdr.seq_no) {
	    report(session, LOG_ERR, ~0,
		   "%s: Illegal sequence number %d (!= %d) for session %.8x",
		   ctx->nas_address_ascii, (int) ctx->hdr.seq_no, (int) session->seq_no, ntohl(ctx->hdr.session_id));
	    cleanup(ctx, cur);
	    return;
	}
    } else {
	if (ctx->hdr.seq_no != 1) {
	    report(NULL, LOG_ERR, ~0,
		   "%s: %s packet (sequence number: %d) for session %.8x", "Stray", ctx->nas_address_ascii, (int) ctx->hdr.seq_no,
		   ntohl(ctx->hdr.session_id));
	    cleanup(ctx, cur);
	    return;
	}

	session = new_session(ctx, &ctx->hdr);
    }

    if (ctx->key && (ctx->in->hdr.flags & TAC_PLUS_UNENCRYPTED_FLAG)) {
	report(NULL, LOG_ERR, ~0,
	       "%s: %s packet (sequence number: %d) for session %.8x", "Unencrypted", ctx->nas_address_ascii, (int) ctx->hdr.seq_no,
	       ntohl(ctx->hdr.session_id));
	cleanup(ctx, cur);
	return;
    }

    if ((ctx->in->hdr.flags & TAC_PLUS_SINGLE_CONNECT_FLAG) && (ctx->single_connection == TRISTATE_YES)) {
	ctx->flags |= TAC_PLUS_SINGLE_CONNECT_FLAG;
	ctx->single_connection_flag = 1;
    }

    char msg[80];
    snprintf(msg, sizeof msg, "Illegal packet (version=0x%.2x type=0x%.2x)", ctx->in->hdr.version, ctx->in->hdr.type);

    int more_keys = 0;
    do {
	int bogus = 0;

	if (more_keys) {
	    md5_xor(&ctx->in->hdr, ctx->key->key, ctx->key->len);
	    ctx->key = ctx->key->next;
	    more_keys = 0;
	}

	if (ctx->key)
	    md5_xor(&ctx->in->hdr, ctx->key->key, ctx->key->len);

	switch (ctx->hdr.type) {
	case TAC_PLUS_AUTHEN:
	    bogus = authen_pak_looks_bogus(&ctx->in->hdr);
	    break;
	case TAC_PLUS_AUTHOR:
	    bogus = author_pak_looks_bogus(&ctx->in->hdr);
	    break;
	case TAC_PLUS_ACCT:
	    bogus = accounting_pak_looks_bogus(&ctx->in->hdr);
	    break;
	default:
	    // Unknown header type, there's no gain in checking secondary keys.
	    ;
	}

	if (bogus && (more_keys = !ctx->key_fixed && ctx->key->next))
	    continue;

	if ((common_data.debug | ctx->debug) & DEBUG_PACKET_FLAG)
	    dump_nas_pak(session, bogus);

	switch (ctx->hdr.type) {

	case TAC_PLUS_AUTHEN:
	    if (!bogus && (ctx->in->hdr.version == TAC_PLUS_VER_DEFAULT || ctx->in->hdr.version == TAC_PLUS_VER_ONE))
		authen(session, &ctx->in->hdr);
	    else
		send_authen_error(session, "%s", msg);
	    break;

	case TAC_PLUS_AUTHOR:
	    if (!bogus && (ctx->in->hdr.version == TAC_PLUS_VER_DEFAULT || (session->bug_compatibility & CLIENT_BUG_BAD_VERSION)))
		author(session, &ctx->in->hdr);
	    else
		send_author_reply(session, TAC_PLUS_AUTHOR_STATUS_ERROR, msg, NULL, 0, NULL);
	    break;

	case TAC_PLUS_ACCT:
	    if (!bogus && (ctx->in->hdr.version == TAC_PLUS_VER_DEFAULT || (session->bug_compatibility & CLIENT_BUG_BAD_VERSION)))
		accounting(session, &ctx->in->hdr);
	    else
		send_acct_reply(session, TAC_PLUS_ACCT_STATUS_ERROR, msg, NULL);
	    break;

	default:
	    report(session, LOG_ERR, ~0, "%s: %s", ctx->nas_address_ascii, msg);
	    cleanup_session(session);
	}
    } while (more_keys);

    if (ctx->key && ctx->key->warn && !ctx->key_fixed && (ctx->key->warn <= io_now.tv_sec))
	report(NULL, LOG_INFO, ~0, "%s uses deprecated key", ctx->nas_address_ascii);

    ctx->key_fixed = 1;
    mem_free(ctx->mem, &ctx->in);
    ctx->hdroff = 0;
}

void tac_write(struct context *ctx, int cur)
{
    ctx->last_io = io_now.tv_sec;
    while (ctx->out) {
	ssize_t len = write(cur,
			    (u_char *) & ctx->out->hdr + ctx->out->offset,
			    ctx->out->length - ctx->out->offset);
	if (len < 0) {
	    if (errno != EAGAIN)
		cleanup(ctx, cur);
	    return;
	}

	ctx->out->offset += len;
	if (ctx->out->offset == ctx->out->length) {
	    tac_pak *n = ctx->out->next;
	    mem_free(ctx->mem, &ctx->out);
	    ctx->out = n;
	}
    }
    io_clr_o(ctx->io, cur);

    // Call shutdown(2) on the socket. This will trigger cleanup() being called via
    // the event loop.
    if (ctx->dying && !ctx->delayed && shutdown(cur, SHUT_WR))
	cleanup(ctx, cur);	// We only get here if shutdown(2) failed.
}

static tac_session *new_session(struct context *ctx, tac_pak_hdr * hdr)
{
    tac_session *session = mem_alloc(ctx->mem, sizeof(tac_session));
    session->ctx = ctx;
    session->debug = ctx->debug;
    session->mem = mem_create(M_LIST);
    session->version = hdr->version;
    session->session_id = hdr->session_id;
    session->seq_no = 1;
    session->timeout = io_now.tv_sec + ctx->realm->session_timeout;
    session->dns_timeout = -1;
    session->final_match = NULL;
    RB_insert(ctx->sessions, session);

    if ((ctx->single_connection == TRISTATE_YES) && !ctx->single_connection_flag) {
	if (ctx->single_connection_test)
	    ctx->single_connection_flag = 1;
	else
	    ctx->single_connection_test = 1;
    }

    if (!session->ctx->aaa_realm->mavis_userdb)
	session->flag_mavis_info = 1;

    report(session, LOG_DEBUG, DEBUG_PACKET_FLAG, "%sNew session%s", common_data.font_blue, common_data.font_plain);

    return session;
}

void cleanup_session(tac_session * session)
{
    struct context *ctx = session->ctx;

    if (session->user && session->user_is_session_specific)
	free_user(session->user);

    if (session->resumefn)
	io_sched_del(ctx->io, session, (void *) resume_session);

    tac_session s = {.session_id = session->session_id };
    RB_search_and_delete(ctx->sessions, &s);
    if (session->mavis_pending && session->mavis_realm && session->mavis_realm->mcx)
	mavis_cancel(session->mavis_realm->mcx, session);
#ifdef WITH_DNS
    if (session->revmap_pending) {
	io_dns_cancel(idc, session);
	if (session->revmap_timedout)
	    add_revmap(&session->nac_address, NULL);
    }
#endif
    mem_destroy(session->mem);
    mem_free(ctx->mem, &session);
    if ((ctx->cleanup_when_idle == TRISTATE_YES)
	&& (!ctx->single_connection_flag || (die_when_idle && !RB_first(ctx->sessions) && !RB_first(ctx->shellctxcache)))) {
	if (ctx->out || ctx->delayed)	// pending output
	    ctx->dying = 1;
	else
	    cleanup(ctx, ctx->sock);
    }
}

/*
 * FreeSWITCH Modular Media Switching Software Library / Soft-Switch Application
 * Copyright (C) 2005-2012, Anthony Minessale II <anthm@freeswitch.org>
 *
 * Version: MPL 1.1
 *
 * The contents of this file are subject to the Mozilla Public License Version
 * 1.1 (the "License"); you may not use this file except in compliance with
 * the License. You may obtain a copy of the License at
 * http://www.mozilla.org/MPL/
 *
 * Software distributed under the License is distributed on an "AS IS" basis,
 * WITHOUT WARRANTY OF ANY KIND, either express or implied. See the License
 * for the specific language governing rights and limitations under the
 * License.
 *
 * The Original Code is FreeSWITCH Modular Media Switching Software Library / Soft-Switch Application
 *
 * The Initial Developer of the Original Code is
 * Anthony Minessale II <anthm@freeswitch.org>
 * Portions created by the Initial Developer are Copyright (C)
 * the Initial Developer. All Rights Reserved.
 *
 * Contributor(s):
 *
 * Anthony Minessale II <anthm@freeswitch.org>
 * Andrew Thompson <andrew@hijacked.us>
 * Rob Charlton <rob.charlton@savageminds.com>
 * Karl Anderson <karl@2600hz.com>
 *
 * Original from mod_erlang_event.
 * ei_helpers.c -- helper functions for ei
 *
 */
#include <switch.h>
#include <ei.h>
#include <sys/types.h>
#include <netinet/in.h>
#include <arpa/nameser.h>
#include <resolv.h>
#include "mod_kazoo.h"

/* Stolen from code added to ei in R12B-5.
 * Since not everyone has this version yet;
 * provide our own version.
 * */

#define put8(s,n) do { \
	(s)[0] = (char)((n) & 0xff); \
	(s) += 1; \
} while (0)

#define put32be(s,n) do {  \
	(s)[0] = ((n) >>  24) & 0xff; \
	(s)[1] = ((n) >>  16) & 0xff; \
	(s)[2] = ((n) >>  8) & 0xff;  \
	(s)[3] = (n) & 0xff; \
	(s) += 4; \
} while (0)

#ifdef EI_DEBUG
static void ei_x_print_reg_msg(ei_x_buff *buf, char *dest, int send) {
    char *mbuf = NULL;
    int i = 1;
	
    ei_s_print_term(&mbuf, buf->buff, &i);
	
    if (send) {
        switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG, "Encoded term %s to '%s'\n", mbuf, dest);
    } else {
        switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG, "Decoded term %s for '%s'\n", mbuf, dest);
    }
	
    free(mbuf);
}

static void ei_x_print_msg(ei_x_buff *buf, erlang_pid *pid, int send) {
    char *pbuf = NULL;
    int i = 0;
    ei_x_buff pidbuf;

    ei_x_new(&pidbuf);
    ei_x_encode_pid(&pidbuf, pid);

    ei_s_print_term(&pbuf, pidbuf.buff, &i);

    ei_x_print_reg_msg(buf, pbuf, send);
    free(pbuf);
}
#endif

void ei_encode_switch_event_headers(ei_x_buff *ebuf, switch_event_t *event) {
    switch_event_header_t *hp;
    char *uuid = switch_event_get_header(event, "unique-id");
    int i;

    for (i = 0, hp = event->headers; hp; hp = hp->next, i++);

    if (event->body)
        i++;

    ei_x_encode_list_header(ebuf, i + 1);

    if (uuid) {
		char *unique_id = switch_event_get_header(event, "unique-id");
		ei_x_encode_binary(ebuf, unique_id, strlen(unique_id));
    } else {
        ei_x_encode_atom(ebuf, "undefined");
    }

    for (hp = event->headers; hp; hp = hp->next) {
        ei_x_encode_tuple_header(ebuf, 2);
        ei_x_encode_binary(ebuf, hp->name, strlen(hp->name));
        switch_url_decode(hp->value);
        ei_x_encode_binary(ebuf, hp->value, strlen(hp->value));
    }

    if (event->body) {
        ei_x_encode_tuple_header(ebuf, 2);
        ei_x_encode_binary(ebuf, "body", strlen("body"));
        ei_x_encode_binary(ebuf, event->body, strlen(event->body));
    }

    ei_x_encode_empty_list(ebuf);
}

void close_socket(switch_socket_t ** sock) {
	if (*sock) {
		switch_socket_shutdown(*sock, SWITCH_SHUTDOWN_READWRITE);
		switch_socket_close(*sock);
		*sock = NULL;
	}
}

void close_socketfd(int *sockfd) {
	if (*sockfd) {
		shutdown(*sockfd, SHUT_RDWR);
        close(*sockfd);
    }
}

switch_socket_t *create_socket(switch_memory_pool_t *pool) {
	switch_sockaddr_t *sa;
	switch_socket_t *socket;

	if(switch_sockaddr_info_get(&sa, globals.ip, SWITCH_UNSPEC, 0, 0, pool)) {
		return NULL;
	}

	if (switch_socket_create(&socket, switch_sockaddr_get_family(sa), SOCK_STREAM, SWITCH_PROTO_TCP, pool)) {
		return NULL;
	}

	if (switch_socket_opt_set(socket, SWITCH_SO_REUSEADDR, 1)) {
		return NULL;
	}

	if (switch_socket_bind(socket, sa)) {
		return NULL;
	}

	if (switch_socket_listen(socket, 5)){
		return NULL;
	}

	//	if (globals.nat_map && switch_nat_get_type()) {
	//		switch_nat_add_mapping(port, SWITCH_NAT_TCP, NULL, SWITCH_FALSE);
	//	}
	
	return socket;
}

switch_status_t create_ei_cnode(const char *ip_addr, const char *name, struct ei_cnode_s *ei_cnode) {
    struct hostent *nodehost;
    char hostname[EI_MAXHOSTNAMELEN + 1] = "";
    char nodename[MAXNODELEN + 1];
    char cnodename[EI_MAXALIVELEN + 1];
    //EI_MAX_COOKIE_SIZE+1
    char *atsign;
		
    /* copy the erlang interface nodename into something we can modify */
    strncpy(cnodename, name, EI_MAXALIVELEN);

    if ((atsign = strchr(cnodename, '@'))) {
        /* we got a qualified node name, don't guess the host/domain */
        snprintf(nodename, MAXNODELEN + 1, "%s", globals.ei_nodename);
        /* truncate the alivename at the @ */
        *atsign = '\0';
    } else {
        if ((nodehost = gethostbyaddr(ip_addr, sizeof (ip_addr), AF_INET))) {
            memcpy(hostname, nodehost->h_name, EI_MAXHOSTNAMELEN);
        }

        if (zstr_buf(hostname) || !strncasecmp(globals.ip, "0.0.0.0", 7)) {
            gethostname(hostname, EI_MAXHOSTNAMELEN);
        }

/*		else {
            if (!(_res.options & RES_INIT)) {
                // init the resolver
                res_init();
            }
            if (_res.dnsrch[0] && !zstr_buf(_res.dnsrch[0])) {
                strncat(hostname, ".", 1);
                strncat(hostname, _res.dnsrch[0], EI_MAXHOSTNAMELEN - strlen(hostname));
            }
}
*/
        snprintf(nodename, MAXNODELEN + 1, "%s@%s", globals.ei_nodename, hostname);
    }

	if (globals.ei_shortname) {
		char *off;
		if ((off = strchr(nodename, '.'))) {
			*off = '\0';
		}
	} 

    /* init the ec stuff */
    if (ei_connect_xinit(ei_cnode, hostname, cnodename, nodename, (Erl_IpAddr) ip_addr, globals.ei_cookie, 0) < 0) {
        switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "Failed to initialize the erlang interface connection structure\n");
        return SWITCH_STATUS_FALSE;
    }

    return SWITCH_STATUS_SUCCESS;
}

switch_status_t ei_compare_pids(const erlang_pid *pid1, const erlang_pid *pid2) {
    if ((!strcmp(pid1->node, pid2->node)) 
		&& pid1->creation == pid2->creation 
		&& pid1->num == pid2->num 
		&& pid1->serial == pid2->serial) {
        return SWITCH_STATUS_SUCCESS;
    } else {
        return SWITCH_STATUS_FALSE;
    }
}

void ei_link(ei_node_t *ei_node, erlang_pid * from, erlang_pid * to) {
    char msgbuf[2048];
    char *s;
    int index = 0;

    index = 5; /* max sizes: */
    ei_encode_version(msgbuf, &index); /*   1 */
    ei_encode_tuple_header(msgbuf, &index, 3);
    ei_encode_long(msgbuf, &index, ERL_LINK);
    ei_encode_pid(msgbuf, &index, from); /* 268 */
    ei_encode_pid(msgbuf, &index, to); /* 268 */

    /* 5 byte header missing */
    s = msgbuf;
    put32be(s, index - 4); /*   4 */
    put8(s, ERL_PASS_THROUGH); /*   1 */
    /* sum:  542 */

    if (write(ei_node->nodefd, msgbuf, index) == -1) {
        switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_WARNING, "Failed to link to process on %s\n", ei_node->peer_nodename);
    }
}

void ei_encode_switch_event(ei_x_buff *ebuf, switch_event_t *event) {
    ei_x_encode_tuple_header(ebuf, 2);
    ei_x_encode_atom(ebuf, "event");
    ei_encode_switch_event_headers(ebuf, event);
}

int ei_helper_send(ei_node_t *ei_node, erlang_pid *to, ei_x_buff *buf) {
    int ret = 0;

    if (ei_node->nodefd) {
#ifdef EI_DEBUG
		ei_x_print_msg(buf, to, 1);
#endif
        ret = ei_send(ei_node->nodefd, to, buf->buff, buf->index);
    }

    return ret;
}

int ei_decode_atom_safe(char *buf, int *index, char *dst) {
    int type, size;

    ei_get_type(buf, index, &type, &size);
	
	if (type != ERL_ATOM_EXT) {
        switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "Unexpected erlang term type %d (size %d), needed atom\n", type, size);
        return -1;
	} else if (size > MAXATOMLEN) {
        switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "Requested decoding of atom with size %d into a buffer of size %d\n", size, MAXATOMLEN);
        return -1;
	} else {
		return ei_decode_atom(buf, index, dst);
	}
}

int ei_decode_string_or_binary(char *buf, int *index, char **dst) {
    int type, size, res;
    long len;

    ei_get_type(buf, index, &type, &size);

    if (type != ERL_STRING_EXT && type != ERL_BINARY_EXT && type != ERL_NIL_EXT) {
        switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "Unexpected erlang term type %d (size %d), needed binary or string\n", type, size);
        return -1;
    }

	*dst = malloc(size + 1);

	if (type == ERL_NIL_EXT) {
		res = 0;
		**dst = '\0';
	} else if (type == ERL_BINARY_EXT) {
        res = ei_decode_binary(buf, index, *dst, &len);
        (*dst)[len] = '\0'; /* binaries aren't null terminated */
    } else {
        res = ei_decode_string(buf, index, *dst);
    }

    return res;
}

int ei_decode_string_or_binary_limited(char *buf, int *index, int maxsize, char *dst) {
    int type, size, res;
    long len;

    ei_get_type(buf, index, &type, &size);

    if (type != ERL_STRING_EXT && type != ERL_BINARY_EXT && type != ERL_NIL_EXT) {
        switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "Unexpected erlang term type %d (size %d), needed binary or string\n", type, size);
        return -1;
    }

	if (size > maxsize) {
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "Requested decoding of %s with size %d into a buffer of size %d\n",
						  type == ERL_BINARY_EXT ? "binary" : "string", size, maxsize);
		return -1;
	}

	if (type == ERL_NIL_EXT) {
		res = 0;
		dst = '\0';
	} else if (type == ERL_BINARY_EXT) {
        res = ei_decode_binary(buf, index, dst, &len);
        dst[len] = '\0'; /* binaries aren't null terminated */
    } else {
        res = ei_decode_string(buf, index, dst);
    }

    return res;
}

switch_event_t *create_default_filter() {
	switch_event_t *filter;

//	switch_event_create_subclass(&filter, SWITCH_EVENT_CUSTOM, "mod_kazoo::filter");
	switch_event_create(&filter, SWITCH_EVENT_CUSTOM);
	switch_event_add_header_string(filter, SWITCH_STACK_BOTTOM, "Acquired-UUID", "undefined");
	switch_event_add_header_string(filter, SWITCH_STACK_BOTTOM, "action", "undefined");
	switch_event_add_header_string(filter, SWITCH_STACK_BOTTOM, "Action", "undefined");
	switch_event_add_header_string(filter, SWITCH_STACK_BOTTOM, "alt_event_type", "undefined");
	switch_event_add_header_string(filter, SWITCH_STACK_BOTTOM, "Answer-State", "undefined");
	switch_event_add_header_string(filter, SWITCH_STACK_BOTTOM, "Application", "undefined");
	switch_event_add_header_string(filter, SWITCH_STACK_BOTTOM, "Application-Data", "undefined");
	switch_event_add_header_string(filter, SWITCH_STACK_BOTTOM, "Application-Name", "undefined");
	switch_event_add_header_string(filter, SWITCH_STACK_BOTTOM, "Application-Response", "undefined");
	switch_event_add_header_string(filter, SWITCH_STACK_BOTTOM, "att_xfer_replaced_by", "undefined");
	switch_event_add_header_string(filter, SWITCH_STACK_BOTTOM, "Auth-Method", "undefined");
	switch_event_add_header_string(filter, SWITCH_STACK_BOTTOM, "Auth-Realm", "undefined");
	switch_event_add_header_string(filter, SWITCH_STACK_BOTTOM, "Auth-User", "undefined");
	switch_event_add_header_string(filter, SWITCH_STACK_BOTTOM, "Bridge-A-Unique-ID", "undefined");
	switch_event_add_header_string(filter, SWITCH_STACK_BOTTOM, "Bridge-B-Unique-ID", "undefined");
	switch_event_add_header_string(filter, SWITCH_STACK_BOTTOM, "Call-Direction", "undefined");
	switch_event_add_header_string(filter, SWITCH_STACK_BOTTOM, "Caller-Callee-ID-Name", "undefined");
	switch_event_add_header_string(filter, SWITCH_STACK_BOTTOM, "Caller-Callee-ID-Number", "undefined");
	switch_event_add_header_string(filter, SWITCH_STACK_BOTTOM, "Caller-Caller-ID-Name", "undefined");
	switch_event_add_header_string(filter, SWITCH_STACK_BOTTOM, "Caller-Caller-ID-Number", "undefined");
	switch_event_add_header_string(filter, SWITCH_STACK_BOTTOM, "Caller-Context", "undefined");
	switch_event_add_header_string(filter, SWITCH_STACK_BOTTOM, "Caller-Controls", "undefined");
	switch_event_add_header_string(filter, SWITCH_STACK_BOTTOM, "Caller-Destination-Number", "undefined");
	switch_event_add_header_string(filter, SWITCH_STACK_BOTTOM, "Caller-Dialplan", "undefined");
	switch_event_add_header_string(filter, SWITCH_STACK_BOTTOM, "Caller-Network-Addr", "undefined");
	switch_event_add_header_string(filter, SWITCH_STACK_BOTTOM, "Caller-Unique-ID", "undefined");
	switch_event_add_header_string(filter, SWITCH_STACK_BOTTOM, "Call-ID", "undefined");
	switch_event_add_header_string(filter, SWITCH_STACK_BOTTOM, "Channel-Call-State", "undefined");
	switch_event_add_header_string(filter, SWITCH_STACK_BOTTOM, "Channel-Call-UUID", "undefined");
	switch_event_add_header_string(filter, SWITCH_STACK_BOTTOM, "Channel-Presence-ID", "undefined");
	switch_event_add_header_string(filter, SWITCH_STACK_BOTTOM, "Channel-State", "undefined");
	switch_event_add_header_string(filter, SWITCH_STACK_BOTTOM, "Chat-Permissions", "undefined");
	switch_event_add_header_string(filter, SWITCH_STACK_BOTTOM, "Conference-Name", "undefined");
	switch_event_add_header_string(filter, SWITCH_STACK_BOTTOM, "Conference-Profile-Name", "undefined");
	switch_event_add_header_string(filter, SWITCH_STACK_BOTTOM, "Conference-Unique-ID", "undefined");
	switch_event_add_header_string(filter, SWITCH_STACK_BOTTOM, "contact", "undefined");
	switch_event_add_header_string(filter, SWITCH_STACK_BOTTOM, "Detected-Tone", "undefined");
	switch_event_add_header_string(filter, SWITCH_STACK_BOTTOM, "dialog_state", "undefined");
	switch_event_add_header_string(filter, SWITCH_STACK_BOTTOM, "direction", "undefined");
	switch_event_add_header_string(filter, SWITCH_STACK_BOTTOM, "Distributed-From", "undefined");
	switch_event_add_header_string(filter, SWITCH_STACK_BOTTOM, "DTMF-Digit", "undefined");
	switch_event_add_header_string(filter, SWITCH_STACK_BOTTOM, "DTMF-Duration", "undefined");
	switch_event_add_header_string(filter, SWITCH_STACK_BOTTOM, "Event-Date-Timestamp", "undefined");
	switch_event_add_header_string(filter, SWITCH_STACK_BOTTOM, "Event-Name", "undefined");
	switch_event_add_header_string(filter, SWITCH_STACK_BOTTOM, "Event-Subclass", "undefined");
	switch_event_add_header_string(filter, SWITCH_STACK_BOTTOM, "expires", "undefined");
	switch_event_add_header_string(filter, SWITCH_STACK_BOTTOM, "Expires", "undefined");
	switch_event_add_header_string(filter, SWITCH_STACK_BOTTOM, "Ext-SIP-IP", "undefined");
	switch_event_add_header_string(filter, SWITCH_STACK_BOTTOM, "File", "undefined");
	switch_event_add_header_string(filter, SWITCH_STACK_BOTTOM, "FreeSWITCH-Hostname", "undefined");
	switch_event_add_header_string(filter, SWITCH_STACK_BOTTOM, "from", "undefined");
	switch_event_add_header_string(filter, SWITCH_STACK_BOTTOM, "Hunt-Destination-Number", "undefined");
	switch_event_add_header_string(filter, SWITCH_STACK_BOTTOM, "ip", "undefined");
	switch_event_add_header_string(filter, SWITCH_STACK_BOTTOM, "Message-Account", "undefined");
	switch_event_add_header_string(filter, SWITCH_STACK_BOTTOM, "metadata", "undefined");
	switch_event_add_header_string(filter, SWITCH_STACK_BOTTOM, "old_node_channel_uuid", "undefined");
	switch_event_add_header_string(filter, SWITCH_STACK_BOTTOM, "Other-Leg-Callee-ID-Name", "undefined");
	switch_event_add_header_string(filter, SWITCH_STACK_BOTTOM, "Other-Leg-Callee-ID-Number", "undefined");
	switch_event_add_header_string(filter, SWITCH_STACK_BOTTOM, "Other-Leg-Caller-ID-Name", "undefined");
	switch_event_add_header_string(filter, SWITCH_STACK_BOTTOM, "Other-Leg-Caller-ID-Number", "undefined");
	switch_event_add_header_string(filter, SWITCH_STACK_BOTTOM, "Other-Leg-Destination-Number", "undefined");
	switch_event_add_header_string(filter, SWITCH_STACK_BOTTOM, "Other-Leg-Direction", "undefined");
	switch_event_add_header_string(filter, SWITCH_STACK_BOTTOM, "Other-Leg-Unique-ID", "undefined");
	switch_event_add_header_string(filter, SWITCH_STACK_BOTTOM, "Participant-Type", "undefined");
	switch_event_add_header_string(filter, SWITCH_STACK_BOTTOM, "Path", "undefined");
	switch_event_add_header_string(filter, SWITCH_STACK_BOTTOM, "profile_name", "undefined");
	switch_event_add_header_string(filter, SWITCH_STACK_BOTTOM, "Profiles", "undefined");
	switch_event_add_header_string(filter, SWITCH_STACK_BOTTOM, "proto-specific-event-name", "undefined");
	switch_event_add_header_string(filter, SWITCH_STACK_BOTTOM, "Raw-Application-Data", "undefined");
	switch_event_add_header_string(filter, SWITCH_STACK_BOTTOM, "realm", "undefined");
	switch_event_add_header_string(filter, SWITCH_STACK_BOTTOM, "Resigning-UUID", "undefined");
	switch_event_add_header_string(filter, SWITCH_STACK_BOTTOM, "set", "undefined");
	switch_event_add_header_string(filter, SWITCH_STACK_BOTTOM, "sip_auto_answer", "undefined");
	switch_event_add_header_string(filter, SWITCH_STACK_BOTTOM, "sip_auth_method", "undefined");
	switch_event_add_header_string(filter, SWITCH_STACK_BOTTOM, "sip_from_host", "undefined");
	switch_event_add_header_string(filter, SWITCH_STACK_BOTTOM, "sip_from_user", "undefined");
	switch_event_add_header_string(filter, SWITCH_STACK_BOTTOM, "sip_to_host", "undefined");
	switch_event_add_header_string(filter, SWITCH_STACK_BOTTOM, "sip_to_user", "undefined");
	switch_event_add_header_string(filter, SWITCH_STACK_BOTTOM, "sub-call-id", "undefined");
	switch_event_add_header_string(filter, SWITCH_STACK_BOTTOM, "technology", "undefined");
	switch_event_add_header_string(filter, SWITCH_STACK_BOTTOM, "to", "undefined");
	switch_event_add_header_string(filter, SWITCH_STACK_BOTTOM, "Unique-ID", "undefined");
	switch_event_add_header_string(filter, SWITCH_STACK_BOTTOM, "URL", "undefined");
	switch_event_add_header_string(filter, SWITCH_STACK_BOTTOM, "username", "undefined");
	switch_event_add_header_string(filter, SWITCH_STACK_BOTTOM, "variable_channel_is_moving", "undefined");
	switch_event_add_header_string(filter, SWITCH_STACK_BOTTOM, "variable_collected_digits", "undefined");
	switch_event_add_header_string(filter, SWITCH_STACK_BOTTOM, "variable_current_application", "undefined");
	switch_event_add_header_string(filter, SWITCH_STACK_BOTTOM, "variable_current_application_data", "undefined");
	switch_event_add_header_string(filter, SWITCH_STACK_BOTTOM, "variable_domain_name", "undefined");
	switch_event_add_header_string(filter, SWITCH_STACK_BOTTOM, "variable_effective_caller_id_name", "undefined");
	switch_event_add_header_string(filter, SWITCH_STACK_BOTTOM, "variable_effective_caller_id_number", "undefined");
	switch_event_add_header_string(filter, SWITCH_STACK_BOTTOM, "variable_fax_bad_rows", "undefined");
	switch_event_add_header_string(filter, SWITCH_STACK_BOTTOM, "variable_fax_document_total_pages", "undefined");
	switch_event_add_header_string(filter, SWITCH_STACK_BOTTOM, "variable_fax_document_transferred_pages", "undefined");
	switch_event_add_header_string(filter, SWITCH_STACK_BOTTOM, "variable_fax_ecm_used", "undefined");
	switch_event_add_header_string(filter, SWITCH_STACK_BOTTOM, "variable_fax_result_code", "undefined");
	switch_event_add_header_string(filter, SWITCH_STACK_BOTTOM, "variable_fax_result_text", "undefined");
	switch_event_add_header_string(filter, SWITCH_STACK_BOTTOM, "variable_fax_success", "undefined");
	switch_event_add_header_string(filter, SWITCH_STACK_BOTTOM, "variable_fax_transfer_rate", "undefined");
	switch_event_add_header_string(filter, SWITCH_STACK_BOTTOM, "variable_holding_uuid", "undefined");
	switch_event_add_header_string(filter, SWITCH_STACK_BOTTOM, "variable_hold_music", "undefined");
	switch_event_add_header_string(filter, SWITCH_STACK_BOTTOM, "variable_media_group_id", "undefined");
	switch_event_add_header_string(filter, SWITCH_STACK_BOTTOM, "variable_originate_disposition", "undefined");
	switch_event_add_header_string(filter, SWITCH_STACK_BOTTOM, "variable_playback_terminator_used", "undefined");
	switch_event_add_header_string(filter, SWITCH_STACK_BOTTOM, "variable_presence_id", "undefined");
	switch_event_add_header_string(filter, SWITCH_STACK_BOTTOM, "variable_record_ms", "undefined");
	switch_event_add_header_string(filter, SWITCH_STACK_BOTTOM, "variable_recovered", "undefined");
	switch_event_add_header_string(filter, SWITCH_STACK_BOTTOM, "variable_silence_hits_exhausted", "undefined");
	switch_event_add_header_string(filter, SWITCH_STACK_BOTTOM, "variable_sip_auth_realm", "undefined");
	switch_event_add_header_string(filter, SWITCH_STACK_BOTTOM, "variable_sip_from_host", "undefined");
	switch_event_add_header_string(filter, SWITCH_STACK_BOTTOM, "variable_sip_from_user", "undefined");
	switch_event_add_header_string(filter, SWITCH_STACK_BOTTOM, "variable_sip_h_X-AUTH-IP", "undefined");
	switch_event_add_header_string(filter, SWITCH_STACK_BOTTOM, "variable_sip_received_ip", "undefined");
	switch_event_add_header_string(filter, SWITCH_STACK_BOTTOM, "variable_sip_to_host", "undefined");
	switch_event_add_header_string(filter, SWITCH_STACK_BOTTOM, "variable_sip_to_user", "undefined");
	switch_event_add_header_string(filter, SWITCH_STACK_BOTTOM, "variable_sofia_profile_name", "undefined");
	switch_event_add_header_string(filter, SWITCH_STACK_BOTTOM, "variable_transfer_history", "undefined");
	switch_event_add_header_string(filter, SWITCH_STACK_BOTTOM, "variable_user_name", "undefined");
	switch_event_add_header_string(filter, SWITCH_STACK_BOTTOM, "variable_endpoint_disposition", "undefined");
	switch_event_add_header_string(filter, SWITCH_STACK_BOTTOM, "variable_originate_disposition", "undefined");
	switch_event_add_header_string(filter, SWITCH_STACK_BOTTOM, "variable_bridge_hangup_cause", "undefined");
	switch_event_add_header_string(filter, SWITCH_STACK_BOTTOM, "variable_hangup_cause", "undefined");
	switch_event_add_header_string(filter, SWITCH_STACK_BOTTOM, "variable_last_bridge_proto_specific_hangup_cause", "undefined");
	switch_event_add_header_string(filter, SWITCH_STACK_BOTTOM, "variable_proto_specific_hangup_cause", "undefined");
	switch_event_add_header_string(filter, SWITCH_STACK_BOTTOM, "VM-Call-ID", "undefined");
	switch_event_add_header_string(filter, SWITCH_STACK_BOTTOM, "VM-sub-call-id", "undefined");
	switch_event_add_header_string(filter, SWITCH_STACK_BOTTOM, "whistle_application_name", "undefined");
	switch_event_add_header_string(filter, SWITCH_STACK_BOTTOM, "whistle_application_response", "undefined");
	switch_event_add_header_string(filter, SWITCH_STACK_BOTTOM, "whistle_event_name", "undefined");
	switch_event_add_header_string(filter, SWITCH_STACK_BOTTOM, "sip_auto_answer_notify", "undefined");
	switch_event_add_header_string(filter, SWITCH_STACK_BOTTOM, "eavesdrop_group", "undefined");
	switch_event_add_header_string(filter, SWITCH_STACK_BOTTOM, "origination_caller_id_name", "undefined");
	switch_event_add_header_string(filter, SWITCH_STACK_BOTTOM, "origination_caller_id_number", "undefined");
	switch_event_add_header_string(filter, SWITCH_STACK_BOTTOM, "origination_callee_id_name", "undefined");
	switch_event_add_header_string(filter, SWITCH_STACK_BOTTOM, "origination_callee_id_number", "undefined");
	switch_event_add_header_string(filter, SWITCH_STACK_BOTTOM, "sip_auth_username", "undefined");
	switch_event_add_header_string(filter, SWITCH_STACK_BOTTOM, "sip_auth_password", "undefined");
	switch_event_add_header_string(filter, SWITCH_STACK_BOTTOM, "effective_caller_id_name", "undefined");
	switch_event_add_header_string(filter, SWITCH_STACK_BOTTOM, "effective_caller_id_number", "undefined");
	switch_event_add_header_string(filter, SWITCH_STACK_BOTTOM, "effective_callee_id_name", "undefined");
	switch_event_add_header_string(filter, SWITCH_STACK_BOTTOM, "effective_callee_id_number", "undefined");

	return filter;
}

/* For Emacs:
 * Local Variables:
 * mode:c
 * indent-tabs-mode:t
 * tab-width:4
 * c-basic-offset:4
 * End:
 * For VIM:
 * vim:set softtabstop=4 shiftwidth=4 tabstop=4:
 */

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
 * Darren Schreiber <d@d-man.org>
 * Mike Jerris <mike@jerris.com>
 * Tamas Cseke <tamas.cseke@virtual-call-center.eu>
 *
 *
 * handle_msg.c -- handle messages received from erlang nodes
 *
 */
#include <switch.h>
#include <ei.h>
#include "mod_kazoo.h"


static switch_status_t handle_msg_event(listener_t *listener, erlang_msg * msg, int arity, ei_x_buff * buf, ei_x_buff * rbuf)
{
	char atom[MAXATOMLEN];

	if (arity == 1) {
		ei_x_encode_tuple_header(rbuf, 2);
		ei_x_encode_atom(rbuf, "error");
		ei_x_encode_atom(rbuf, "badarg");
	} else {
		int custom = 0;
		switch_event_types_t type;
		int i = 0;

		for (i = 1; i < arity; i++) {
			if (!ei_decode_atom(buf->buff, &buf->index, atom)) {
				if (custom) {
					switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG, "Bind custom event %s to %s <%d.%d.%d>\n", atom, msg->from.node, msg->from.creation, msg->from.num, msg->from.serial);
					/* TODO: figure out what to do with custom events.... */
				} else if (switch_name_event(atom, &type) == SWITCH_STATUS_SUCCESS) {
					if (type == SWITCH_EVENT_ALL) {
						switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG, "Bind ALL events to %s <%d.%d.%d>\n", msg->from.node, msg->from.creation, msg->from.num, msg->from.serial);
						for (uint32_t x = 0; x < SWITCH_EVENT_ALL; x++) {
							add_event_binding(listener, &type, &msg->from);
						}
					}
					if (type <= SWITCH_EVENT_ALL) {
						switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG, "Bind event %s to %s <%d.%d.%d>\n", atom, msg->from.node, msg->from.creation, msg->from.num, msg->from.serial);
						add_event_binding(listener, &type, &msg->from);
					}
					if (type == SWITCH_EVENT_CUSTOM) {
						custom++;
					}
				}
			}
		}
		ei_x_encode_atom(rbuf, "ok");
	}
	return SWITCH_STATUS_SUCCESS;
}

static switch_status_t handle_msg_tuple(listener_t *listener, erlang_msg * msg, ei_x_buff * buf, ei_x_buff * rbuf)
{
	char tupletag[MAXATOMLEN];
	int arity;
	switch_status_t ret = SWITCH_STATUS_SUCCESS;

	ei_decode_tuple_header(buf->buff, &buf->index, &arity);
	if (ei_decode_atom(buf->buff, &buf->index, tupletag)) {
		ei_x_encode_tuple_header(rbuf, 2);
		ei_x_encode_atom(rbuf, "error");
		ei_x_encode_atom(rbuf, "badarg");
	} else {
		if (!strncmp(tupletag, "event", MAXATOMLEN)) {
			ret = handle_msg_event(listener, msg, arity, buf, rbuf);
			/*		} else if (!strncmp(tupletag, "fetch_reply", MAXATOMLEN)) {
					ret = handle_msg_fetch_reply(listener, buf, rbuf);
					} else if (!strncmp(tupletag, "set_log_level", MAXATOMLEN)) {
					ret = handle_msg_set_log_level(listener, arity, buf, rbuf);
					} else if (!strncmp(tupletag, "session_event", MAXATOMLEN)) {
					ret = handle_msg_session_event(listener, msg, arity, buf, rbuf);
					} else if (!strncmp(tupletag, "nixevent", MAXATOMLEN)) {
					ret = handle_msg_nixevent(listener, arity, buf, rbuf);
					} else if (!strncmp(tupletag, "session_nixevent", MAXATOMLEN)) {
					ret = handle_msg_session_nixevent(listener, msg, arity, buf, rbuf);
					} else if (!strncmp(tupletag, "api", MAXATOMLEN)) {
					ret = handle_msg_api(listener, msg, arity, buf, rbuf);
					} else if (!strncmp(tupletag, "bgapi", MAXATOMLEN)) {
					ret = handle_msg_bgapi(listener, msg, arity, buf, rbuf);
					} else if (!strncmp(tupletag, "sendevent", MAXATOMLEN)) {
					ret = handle_msg_sendevent(listener, arity, buf, rbuf);
					} else if (!strncmp(tupletag, "sendmsg", MAXATOMLEN)) {
					ret = handle_msg_sendmsg(listener, arity, buf, rbuf);
					} else if (!strncmp(tupletag, "bind", MAXATOMLEN)) {
					ret = handle_msg_bind(listener, msg, buf, rbuf);
					} else if (!strncmp(tupletag, "handlecall", MAXATOMLEN)) {
					ret = handle_msg_handlecall(listener, msg, arity, buf, rbuf);
					} else if (!strncmp(tupletag, "rex", MAXATOMLEN)) {
					ret = handle_msg_rpcresponse(listener, msg, arity, buf, rbuf);
					} else if (!strncmp(tupletag, "setevent", MAXATOMLEN)) {
					ret = handle_msg_setevent(listener, msg, arity, buf, rbuf);
					} else if (!strncmp(tupletag, "session_setevent", MAXATOMLEN)) {
					ret = handle_msg_session_setevent(listener, msg, arity, buf, rbuf); */
	} else {
		ei_x_encode_tuple_header(rbuf, 2);
		ei_x_encode_atom(rbuf, "error");
		ei_x_encode_atom(rbuf, "undef");
	}
	}
	return ret;
}

/* fake enough of the net_kernel module to be able to respond to net_adm:ping */
/* {'$gen_call', {<cpx@freecpx.128.0>, #Ref<254770.4.0>}, {is_auth, cpx@freecpx} */
static switch_status_t handle_net_kernel_msg(listener_t *listener, erlang_msg * msg, ei_x_buff * buf, ei_x_buff * rbuf)
{
	int version, size, type, arity;
	char atom[MAXATOMLEN];
	erlang_ref ref;
	erlang_pid pid;

	buf->index = 0;
	ei_decode_version(buf->buff, &buf->index, &version);
	ei_get_type(buf->buff, &buf->index, &type, &size);

	/* is_tuple(Buff) */
	if (type != ERL_SMALL_TUPLE_EXT) {
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG, "Received net_kernel message of an unexpected type\n");
		return SWITCH_STATUS_FALSE;
	}

	ei_decode_tuple_header(buf->buff, &buf->index, &arity);

	/* {_, _, _} = Buf */
	if (arity != 3) {
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG, "Received net_kernel tuple has an unexpected arity\n");
		return SWITCH_STATUS_FALSE;
	}

	/* {'$gen_call', _, _} = Buf */
	if (ei_decode_atom(buf->buff, &buf->index, atom) || strncmp(atom, "$gen_call", MAXATOMLEN)) {
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG, "Received net_kernel message tuple does not begin with the atom '$gen_call'\n");
		return SWITCH_STATUS_FALSE;
	}

	ei_get_type(buf->buff, &buf->index, &type, &size);

	/* {_, Sender, _}=Buff, is_tuple(Sender) */
	if (type != ERL_SMALL_TUPLE_EXT) {
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG, "Second element of the net_kernel tuple is an unexpected type\n");
		return SWITCH_STATUS_FALSE;
	}

	ei_decode_tuple_header(buf->buff, &buf->index, &arity);

	/* {_, _}=Sender */
	if (arity != 2) {
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG, "Second element of the net_kernel message has an unexpected arity\n");
		return SWITCH_STATUS_FALSE;
	}

	/* {Pid, Ref}=Sender */
	if (ei_decode_pid(buf->buff, &buf->index, &pid) || ei_decode_ref(buf->buff, &buf->index, &ref)) {
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG, "Unable to decode erlang pid or ref of the net_kernel tuple second element\n");
		return SWITCH_STATUS_FALSE;
	}

	ei_get_type(buf->buff, &buf->index, &type, &size);

	/* {_, _, Request}=Buff, is_tuple(Request) */
	if (type != ERL_SMALL_TUPLE_EXT) {
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG, "Third element of the net_kernel message is an unexpected type\n");
		return SWITCH_STATUS_FALSE;
	}

	ei_decode_tuple_header(buf->buff, &buf->index, &arity);

	/* {_, _}=Request */
	if (arity != 2) {
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG, "Third element of the net_kernel message has an unexpected arity\n");
		return SWITCH_STATUS_FALSE;
	}

	/* {is_auth, _}=Request */
	if (ei_decode_atom(buf->buff, &buf->index, atom) || strncmp(atom, "is_auth", MAXATOMLEN)) {
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG, "The net_kernel message third element does not begin with the atom 'is_auth'\n");
		return SWITCH_STATUS_FALSE;
	}

	/* To ! {Tag, Reply} */
	ei_x_encode_tuple_header(rbuf, 2);
	ei_x_encode_ref(rbuf, &ref);
	ei_x_encode_atom(rbuf, "yes");

	ei_helper_send(listener, &pid, rbuf->buff, rbuf->index);

	return SWITCH_STATUS_NOOP;
}

switch_status_t handle_msg(listener_t *listener, erlang_msg * msg, ei_x_buff * buf, ei_x_buff * rbuf)
{
	int type, type2, size, version, arity, tmpindex;
	switch_status_t ret = SWITCH_STATUS_SUCCESS;

	if (msg->msgtype == ERL_REG_SEND && !strncmp(msg->toname, "net_kernel", MAXATOMLEN)) {
		/* try to respond to ping stuff */
		ret = handle_net_kernel_msg(listener, msg, buf, rbuf);
	} else {
		buf->index = 0;
		ei_decode_version(buf->buff, &buf->index, &version);
		ei_get_type(buf->buff, &buf->index, &type, &size);

		switch (type) {
			case ERL_SMALL_TUPLE_EXT:
			case ERL_LARGE_TUPLE_EXT:
				tmpindex = buf->index;
				ei_decode_tuple_header(buf->buff, &tmpindex, &arity);
				ei_get_type(buf->buff, &tmpindex, &type2, &size);

				switch (type2) {
					case ERL_ATOM_EXT:
						ret = handle_msg_tuple(listener, msg, buf, rbuf);
						break;
					case ERL_REFERENCE_EXT:
					case ERL_NEW_REFERENCE_EXT:
						//				ret = handle_ref_tuple(listener, msg, buf, rbuf);
						break;
					default:
						switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_NOTICE, "Received unexpected erlang tuple, first element of type %d\n", type2);
						/* some other kind of erlang term */
						ei_x_encode_tuple_header(rbuf, 2);
						ei_x_encode_atom(rbuf, "error");
						ei_x_encode_atom(rbuf, "undef");
						break;
				}

				break;

			case ERL_ATOM_EXT:
				//			ret = handle_msg_atom(listener, msg, buf, rbuf);
				break;

			default:
				/* some other kind of erlang term */
				switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_NOTICE, "Received unexpected erlang term, started with type %d\n", type);
				ei_x_encode_tuple_header(rbuf, 2);
				ei_x_encode_atom(rbuf, "error");
				ei_x_encode_atom(rbuf, "undef");
				break;
		}
	}

	/* TODO: tmp debug line but should be added to an API call for listing pid bindings... */
	list_event_bindings(listener);

	if (ret == SWITCH_STATUS_NOOP) {
		ret = SWITCH_STATUS_SUCCESS;
	} else if (rbuf->index > 1) {
		ei_helper_send(listener, &msg->from, rbuf->buff, rbuf->index);
	}

	return ret;
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

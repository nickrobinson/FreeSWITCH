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

struct api_command_struct {
	char *api_cmd;
	char *arg;
	listener_t *listener;
	char uuid_str[SWITCH_UUID_FORMATTED_LENGTH + 1];
	uint8_t bg;
	erlang_pid pid;
	switch_memory_pool_t *pool;
};

static void *SWITCH_THREAD_FUNC api_exec(switch_thread_t *thread, void *obj)
{
	switch_bool_t r = SWITCH_TRUE;
	struct api_command_struct *acs = (struct api_command_struct *) obj;
	switch_stream_handle_t stream = { 0 };
	char *reply, *freply = NULL;
	switch_status_t status;
	listener_t *listener = NULL;

	if (!acs || !acs->listener) {
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "Internal error\n");
		return NULL;
	}

	listener = acs->listener;

	if(!switch_test_flag(listener, LFLAG_RUNNING) || !switch_test_flag(&globals, LFLAG_RUNNING)) {
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "Ignoring command while shuting down\n");
		return NULL;
	}

	if(switch_thread_rwlock_rdlock(listener->rwlock) != SWITCH_STATUS_SUCCESS) {
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "Unable to get read lock for listener\n");
		goto done;
	}

	SWITCH_STANDARD_STREAM(stream);

	if ((status = switch_api_execute(acs->api_cmd, acs->arg, NULL, &stream)) == SWITCH_STATUS_SUCCESS) {
		reply = stream.data;
	} else {
		freply = switch_mprintf("%s: Command not found!\n", acs->api_cmd);
		reply = freply;
		r = SWITCH_FALSE;
	}

	if (!reply) {
		reply = "Command returned no output!";
		r = SWITCH_FALSE;
	}

	if (*reply == '-')
		r = SWITCH_FALSE;

	if (acs->bg) {
		switch_event_t *event;

		if (switch_event_create(&event, SWITCH_EVENT_BACKGROUND_JOB) == SWITCH_STATUS_SUCCESS) {
			ei_x_buff ebuf;

			switch_event_add_header_string(event, SWITCH_STACK_BOTTOM, "Job-UUID", acs->uuid_str);
			switch_event_add_header_string(event, SWITCH_STACK_BOTTOM, "Job-Command", acs->api_cmd);

			ei_x_new_with_version(&ebuf);

			if (acs->arg) {
				switch_event_add_header_string(event, SWITCH_STACK_BOTTOM, "Job-Command-Arg", acs->arg);
			}

			switch_event_add_header_string(event, SWITCH_STACK_BOTTOM, "Job-Successful", r ? "true" : "false");
			switch_event_add_body(event, "%s", reply);

			switch_event_fire(&event);

			switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG, "Sending bgapi reply to %s\n", acs->pid.node);

			ei_x_encode_tuple_header(&ebuf, 3);

			if (r)
				ei_x_encode_atom(&ebuf, "bgok");
			else
				ei_x_encode_atom(&ebuf, "bgerror");

			_ei_x_encode_string(&ebuf, acs->uuid_str);
			_ei_x_encode_string(&ebuf, reply);

			ei_helper_send(listener, &acs->pid, &ebuf);

			ei_x_free(&ebuf);
		}
	} else {
		ei_x_buff rbuf;
		ei_x_new_with_version(&rbuf);
		ei_x_encode_tuple_header(&rbuf, 2);

		if (!strlen(reply)) {
			reply = "Command returned no output!";
			r = SWITCH_FALSE;
		}

		if (r) {
			ei_x_encode_atom(&rbuf, "ok");
		} else {
			ei_x_encode_atom(&rbuf, "error");
		}

		_ei_x_encode_string(&rbuf, reply);

		ei_helper_send(listener, &acs->pid, &rbuf);

		ei_x_free(&rbuf);
	}

	switch_safe_free(stream.data);
	switch_safe_free(freply);

	switch_thread_rwlock_unlock(listener->rwlock);

  done:
	if (acs->bg) {
		switch_memory_pool_t *pool = acs->pool;
		acs = NULL;
		switch_core_destroy_memory_pool(&pool);
		pool = NULL;
	}
	return NULL;
}

static switch_status_t handle_msg_nixevent(listener_t *listener, erlang_msg * msg, int arity, ei_x_buff * buf, ei_x_buff * rbuf) {
    char atom[MAXATOMLEN];

    if (arity == 1) {
        ei_x_encode_tuple_header(rbuf, 2);
        ei_x_encode_atom(rbuf, "error");
        ei_x_encode_atom(rbuf, "badarg");
    } else {
        switch_event_types_t type;
        int custom = 0;

        for (int i = 1; i < arity; i++) {
            if (!ei_decode_atom(buf->buff, &buf->index, atom)) {

                if (custom) {
                    /* TODO: figure out what to do with custom events... */
                } else if (switch_name_event(atom, &type) == SWITCH_STATUS_SUCCESS) {
                    switch (type) {
                        case SWITCH_EVENT_CUSTOM:
                            custom++;
                            break;
                        case SWITCH_EVENT_ALL:
                            remove_pid_from_event_bindings(listener, &msg->from);
                            break;
                        default:
                            remove_pid_from_event_binding(listener, &type, &msg->from);
                    }
                }
            }
        }

        ei_x_encode_atom(rbuf, "ok");
    }

    return SWITCH_STATUS_SUCCESS;
}

static switch_status_t handle_msg_event(listener_t *listener, erlang_msg * msg, int arity, ei_x_buff * buf, ei_x_buff * rbuf) {
    char atom[MAXATOMLEN];

    if (arity == 1) {
        ei_x_encode_tuple_header(rbuf, 2);
        ei_x_encode_atom(rbuf, "error");
        ei_x_encode_atom(rbuf, "badarg");
    } else {
        switch_event_types_t type;
        int custom = 0;

        for (int i = 1; i < arity; i++) {
            if (!ei_decode_atom(buf->buff, &buf->index, atom)) {
                if (custom) {
                    /* TODO: figure out what to do with custom events.... */
                } else if (switch_name_event(atom, &type) == SWITCH_STATUS_SUCCESS) {
                    switch (type) {
                        case SWITCH_EVENT_CUSTOM:
                            custom++;
                            break;
                        case SWITCH_EVENT_ALL:
                            for (switch_event_types_t x = 0; x <= SWITCH_EVENT_ALL; x++) {
                                add_event_binding(listener, &x, &msg->from);
                            }
                            break;
                        default:
                            add_event_binding(listener, &type, &msg->from);
                    }
                }
            }
        }

        ei_x_encode_atom(rbuf, "ok");
    }

    return SWITCH_STATUS_SUCCESS;
}

static switch_status_t handle_msg_fetch_reply(listener_t *listener, ei_x_buff * buf, ei_x_buff * rbuf) {
	int type, size;
    char uuid_str[SWITCH_UUID_FORMATTED_LENGTH + 1];
	char *xml_str;
	switch_xml_t xml = NULL;
	xml_fetch_msg_t *fetch_msg = NULL;

	/* Try to get the fetch uuid string */
	ei_get_type(buf->buff, &buf->index, &type, &size);

	if (type != ERL_NIL_EXT && type != ERL_STRING_EXT && type != ERL_BINARY_EXT) {
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_WARNING, "XML fetch response did not contain decodable uuid (was type %d of size %d)\n", type, size);
		ei_x_encode_tuple_header(rbuf, 2);
		ei_x_encode_atom(rbuf, "error");
		ei_x_encode_atom(rbuf, "badarg");
		return SWITCH_STATUS_SUCCESS;
	}

	ei_decode_string_or_binary(buf->buff, &buf->index, SWITCH_UUID_FORMATTED_LENGTH, uuid_str);

	if (!(fetch_msg = switch_core_hash_find_locked(globals.fetch_resp_hash, uuid_str, globals.fetch_resp_mutex))) {
		ei_x_encode_tuple_header(rbuf, 2);
		ei_x_encode_atom(rbuf, "error");
		ei_x_encode_atom(rbuf, "baduuid");
		return SWITCH_STATUS_SUCCESS;
	}

	if (switch_mutex_trylock(fetch_msg->mutex) != SWITCH_STATUS_SUCCESS) {
		ei_x_encode_tuple_header(rbuf, 2);
		ei_x_encode_atom(rbuf, "error");
		ei_x_encode_atom(rbuf, "baduuid");
		return SWITCH_STATUS_SUCCESS;
	}

	/* Try to get the XML string */
	ei_get_type(buf->buff, &buf->index, &type, &size);

	if (type == ERL_NIL_EXT && type != ERL_STRING_EXT && type != ERL_BINARY_EXT) {
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_WARNING, "XML fetch response did not contain decodable XML (was type %d of size %d)\n", type, size);
		ei_x_encode_tuple_header(rbuf, 2);
		ei_x_encode_atom(rbuf, "error");
		ei_x_encode_atom(rbuf, "badarg");
		return SWITCH_STATUS_SUCCESS;
	}

	if (!(xml_str = malloc(size + 1))) {
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_CRIT, "Memory Error\n");
		return SWITCH_STATUS_NOOP;
	}

	ei_decode_string_or_binary(buf->buff, &buf->index, size, xml_str);

	/* If we succeed in parsing the xml_str, it will be free'd by the core */
	if ((xml = switch_xml_parse_str_dynamic(xml_str, SWITCH_FALSE))) {
		fetch_msg->xml = xml;
		switch_thread_cond_signal(fetch_msg->response_available);
		switch_mutex_unlock(fetch_msg->mutex);
		ei_x_encode_atom(rbuf, "ok");
	} else {
		switch_safe_free(xml_str);
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "Error parsing fetch response XML\n");
		ei_x_encode_tuple_header(rbuf, 2);
		ei_x_encode_atom(rbuf, "error");
		ei_x_encode_atom(rbuf, "badarg");
	}

    return SWITCH_STATUS_SUCCESS;
}

static switch_status_t handle_msg_api(listener_t *listener, erlang_msg *msg, int arity, ei_x_buff *buf, ei_x_buff *rbuf) {
	struct api_command_struct acs = { 0 };
	char api_cmd[MAXATOMLEN];
	int type;
	int size;
	char *arg;

	if (arity < 3) {
		ei_x_encode_tuple_header(rbuf, 2);
		ei_x_encode_atom(rbuf, "error");
		ei_x_encode_atom(rbuf, "badarg");
		return SWITCH_STATUS_SUCCESS;
	}

	ei_get_type(buf->buff, &buf->index, &type, &size);

	if ((size > (sizeof(api_cmd) - 1)) || ei_decode_atom(buf->buff, &buf->index, api_cmd)) {
		ei_x_encode_tuple_header(rbuf, 2);
		ei_x_encode_atom(rbuf, "error");
		ei_x_encode_atom(rbuf, "badarg");
		return SWITCH_STATUS_SUCCESS;
	}

	ei_get_type(buf->buff, &buf->index, &type, &size);
	arg = malloc(size + 1);

	if (ei_decode_string(buf->buff, &buf->index, arg)) {
		switch_safe_free(arg);
		ei_x_encode_tuple_header(rbuf, 2);
		ei_x_encode_atom(rbuf, "error");
		ei_x_encode_atom(rbuf, "badarg");
		return SWITCH_STATUS_SUCCESS;
	}

	acs.listener = listener;
	acs.api_cmd = api_cmd;
	acs.arg = arg;
	acs.bg = 0;
	acs.pid = msg->from;
	api_exec(NULL, (void *) &acs);
	
	switch_safe_free(arg);
	return SWITCH_STATUS_NOOP;
}

/* TODO: REFACTOR */
static switch_status_t handle_msg_bgapi(listener_t *listener, erlang_msg * msg, int arity, ei_x_buff * buf, ei_x_buff * rbuf) {
    return SWITCH_STATUS_SUCCESS;
}

/* TODO: REFACTOR */
static switch_status_t handle_msg_sendevent(listener_t *listener, int arity, ei_x_buff * buf, ei_x_buff * rbuf) {
    return SWITCH_STATUS_SUCCESS;
}

/* TODO: REFACTOR */
static switch_status_t handle_msg_sendmsg(listener_t *listener, int arity, ei_x_buff * buf, ei_x_buff * rbuf) {
    return SWITCH_STATUS_SUCCESS;
}

static switch_status_t handle_msg_bind(listener_t *listener, erlang_msg * msg, ei_x_buff * buf, ei_x_buff * rbuf) {
	char section_str[MAXATOMLEN];
	switch_xml_section_t section;

	if (ei_decode_atom(buf->buff, &buf->index, section_str) || !(section = switch_xml_parse_section_string(section_str))) {
		ei_x_encode_tuple_header(rbuf, 2);
		ei_x_encode_atom(rbuf, "error");
		ei_x_encode_atom(rbuf, "badarg");
	} else {
		add_fetch_binding(listener, section_str, &msg->from);
		ei_x_encode_atom(rbuf, "ok");
	}

	return SWITCH_STATUS_SUCCESS;
}

/* NOTE: this does NOT support mod_erlang_event handlecall/3, pid only */
static switch_status_t handle_msg_handlecall(listener_t *listener, erlang_msg * msg, int arity, ei_x_buff * buf, ei_x_buff * rbuf) {
    char uuid_str[SWITCH_UUID_FORMATTED_LENGTH + 1];

    if (arity == 2 && !ei_decode_string_or_binary(buf->buff, &buf->index, SWITCH_UUID_FORMATTED_LENGTH, uuid_str)) {
        switch_core_session_t *session;
        if (!zstr_buf(uuid_str)) {
            if ((session = switch_core_session_locate(uuid_str))) {
                /* release the lock returned by session locate */
                switch_core_session_rwunlock(session);
                //                add_session_binding(listener, msg->from);
                ei_x_encode_atom(rbuf, "ok");
            } else {
                ei_x_encode_tuple_header(rbuf, 2);
                ei_x_encode_atom(rbuf, "error");
                ei_x_encode_atom(rbuf, "baduuid");
            }
        } else {
            ei_x_encode_tuple_header(rbuf, 2);
            ei_x_encode_atom(rbuf, "error");
            ei_x_encode_atom(rbuf, "baduuid");
        }
    } else {
        ei_x_encode_tuple_header(rbuf, 2);
        ei_x_encode_atom(rbuf, "error");
        ei_x_encode_atom(rbuf, "badarg");
    }

    return SWITCH_STATUS_SUCCESS;
}

static switch_status_t handle_msg_tuple(listener_t *listener, erlang_msg * msg, ei_x_buff * buf, ei_x_buff * rbuf) {
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
        } else if (!strncmp(tupletag, "nixevent", MAXATOMLEN)) {
            ret = handle_msg_nixevent(listener, msg, arity, buf, rbuf);
//        } else if (!strncmp(tupletag, "setevent", MAXATOMLEN)) {
//            ret = handle_msg_setevent(listener, msg, arity, buf, rbuf);
            
        } else if (!strncmp(tupletag, "handlecall", MAXATOMLEN)) {
            ret = handle_msg_handlecall(listener, msg, arity, buf, rbuf);
//        } else if (!strncmp(tupletag, "session_event", MAXATOMLEN)) {
//            ret = handle_msg_session_event(listener, msg, arity, buf, rbuf);
//        } else if (!strncmp(tupletag, "session_nixevent", MAXATOMLEN)) {
//            ret = handle_msg_session_nixevent(listener, msg, arity, buf, rbuf);
//        } else if (!strncmp(tupletag, "session_setevent", MAXATOMLEN)) {
//            ret = handle_msg_session_setevent(listener, msg, arity, buf, rbuf);
            
        } else if (!strncmp(tupletag, "bind", MAXATOMLEN)) {
            ret = handle_msg_bind(listener, msg, buf, rbuf);
        } else if (!strncmp(tupletag, "fetch_reply", MAXATOMLEN)) {
            ret = handle_msg_fetch_reply(listener, buf, rbuf);

//        } else if (!strncmp(tupletag, "set_log_level", MAXATOMLEN)) {
//            ret = handle_msg_set_log_level(listener, arity, buf, rbuf);

        } else if (!strncmp(tupletag, "api", MAXATOMLEN)) {
            ret = handle_msg_api(listener, msg, arity, buf, rbuf);
        } else if (!strncmp(tupletag, "bgapi", MAXATOMLEN)) {
            ret = handle_msg_bgapi(listener, msg, arity, buf, rbuf);
        } else if (!strncmp(tupletag, "sendevent", MAXATOMLEN)) {
            ret = handle_msg_sendevent(listener, arity, buf, rbuf);
        } else if (!strncmp(tupletag, "sendmsg", MAXATOMLEN)) {
            ret = handle_msg_sendmsg(listener, arity, buf, rbuf);


//        } else if (!strncmp(tupletag, "rex", MAXATOMLEN)) {
//            ret = handle_msg_rpcresponse(listener, msg, arity, buf, rbuf);
        } else {
            ei_x_encode_tuple_header(rbuf, 2);
            ei_x_encode_atom(rbuf, "error");
            ei_x_encode_atom(rbuf, "badarg");
        }
    }

    return ret;
}

static switch_status_t handle_msg_atom(listener_t *listener, erlang_msg * msg, ei_x_buff * buf, ei_x_buff * rbuf) {
    char atom[MAXATOMLEN];
    switch_status_t ret = SWITCH_STATUS_SUCCESS;

    if (ei_decode_atom(buf->buff, &buf->index, atom)) {
        ei_x_encode_tuple_header(rbuf, 2);
        ei_x_encode_atom(rbuf, "error");
        ei_x_encode_atom(rbuf, "badarg");
    } else if (!strncmp(atom, "nolog", MAXATOMLEN)) {
        /* TODO: remove logging bindings */
        ei_x_encode_atom(rbuf, "ok");
    } else if (!strncmp(atom, "register_log_handler", MAXATOMLEN)) {
        /* TODO: register log handler */
        ei_x_encode_atom(rbuf, "ok");
    } else if (!strncmp(atom, "register_event_handler", MAXATOMLEN)) {
        /* TODO: register event handler */
        ei_x_encode_atom(rbuf, "ok");
    } else if (!strncmp(atom, "noevents", MAXATOMLEN)) {
        remove_pid_from_event_bindings(listener, &msg->from);
        ei_x_encode_atom(rbuf, "ok");
    } else if (!strncmp(atom, "session_noevents", MAXATOMLEN)) {
        /* TODO: remove all session bindings for pid */
        ei_x_encode_atom(rbuf, "ok");
    } else if (!strncmp(atom, "exit", MAXATOMLEN)) {
        ei_x_encode_atom(rbuf, "ok");
        ret = SWITCH_STATUS_TERM;
    } else if (!strncmp(atom, "getpid", MAXATOMLEN)) {
        ei_x_encode_tuple_header(rbuf, 2);
        ei_x_encode_atom(rbuf, "ok");
        ei_x_encode_pid(rbuf, ei_self(listener->ec));
    } else if (!strncmp(atom, "link", MAXATOMLEN)) {
        ei_link(listener, ei_self(listener->ec), &msg->from);
        ret = SWITCH_STATUS_FALSE;
    } else {
        ei_x_encode_tuple_header(rbuf, 2);
        ei_x_encode_atom(rbuf, "error");
        ei_x_encode_atom(rbuf, "badarg");
    }

    return ret;
}

/* fake enough of the net_kernel module to be able to respond to net_adm:ping */
static switch_status_t handle_net_kernel_msg(listener_t *listener, erlang_msg * msg, ei_x_buff * buf, ei_x_buff * rbuf) {
    int version, size, type, arity;
    char atom[MAXATOMLEN];
    erlang_ref ref;
    erlang_pid pid;

	switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG, "Received net_kernel message, attempting to reply\n");

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

    ei_helper_send(listener, &pid, rbuf);

    return SWITCH_STATUS_NOOP;
}

switch_status_t handle_msg(listener_t *listener, erlang_msg * msg, ei_x_buff * buf, ei_x_buff * rbuf) {
    int type, type2, size, version, arity, tmpindex;
    switch_status_t ret = SWITCH_STATUS_SUCCESS;

    if (msg->msgtype == ERL_REG_SEND && !strncmp(msg->toname, "net_kernel", MAXATOMLEN)) {
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
                        //ret = handle_ref_tuple(listener, msg, buf, rbuf);
                        break;
                    default:
                        /* unexpected erlang tuple */
                        switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_NOTICE, "Received unexpected erlang tuple, first element of type %d\n", type2);
                        ei_x_encode_tuple_header(rbuf, 2);
                        ei_x_encode_atom(rbuf, "error");
                        ei_x_encode_atom(rbuf, "badarg");
                        break;
                }
                break;
            case ERL_ATOM_EXT:
                ret = handle_msg_atom(listener, msg, buf, rbuf);
                break;
            default:
                /* some other kind of erlang term */
                switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_NOTICE, "Received unexpected erlang term, started with type %d\n", type);
                ei_x_encode_tuple_header(rbuf, 2);
                ei_x_encode_atom(rbuf, "error");
                ei_x_encode_atom(rbuf, "badarg");
                break;
        }
    }

    /* TODO: tmp debug line but should be added to an API call for listing pid bindings... */
//    list_event_bindings(listener);

    if (ret == SWITCH_STATUS_NOOP) {
        ret = SWITCH_STATUS_SUCCESS;
    } else if (rbuf->index > 1) {
        ei_helper_send(listener, &msg->from, rbuf);
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

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
 *
 *
 *
 */
#include <switch.h>
#include <ei.h>
#include "mod_kazoo.h"

SWITCH_HASH_DELETE_FUNC(bindings_pid_cleanup_callback)
{
    erlang_pid *pid = (erlang_pid *) val;
    
    switch_safe_free(pid);

    return SWITCH_TRUE;
}

switch_status_t send_fetch_to_bindings(listener_t *listener, char *uuid_str) {
	xml_fetch_msg_t *fetch_msg = NULL;
	ei_x_buff ebuf;
	const char *section;
	const char *tag_name;
	const char *key_name;
	const char *key_value;
    switch_hash_t *bindings;
    switch_hash_index_t *binding;

	switch_mutex_lock(globals.fetch_resp_mutex);
	if (!(fetch_msg = switch_core_hash_find(globals.fetch_resp_hash, uuid_str))) {
		switch_mutex_unlock(globals.fetch_resp_mutex);
		return SWITCH_STATUS_SUCCESS;
	}

	section = fetch_msg->section;
	tag_name = fetch_msg->tag_name;
	key_name = fetch_msg->key_name;
	key_value = fetch_msg->key_value;

	ei_x_new_with_version(&ebuf);
	
	ei_x_encode_tuple_header(&ebuf, 7);
	ei_x_encode_atom(&ebuf, "fetch");
	ei_x_encode_atom(&ebuf, section);
	_ei_x_encode_string(&ebuf, tag_name ? tag_name : "undefined");
	_ei_x_encode_string(&ebuf, key_name ? key_name : "undefined");
	_ei_x_encode_string(&ebuf, key_value ? key_value : "undefined");
	_ei_x_encode_string(&ebuf, uuid_str);
	
	if (fetch_msg->params) {
		ei_encode_switch_event_headers(&ebuf, fetch_msg->params);
	} else {
		ei_x_encode_empty_list(&ebuf);
	}
	switch_mutex_unlock(globals.fetch_resp_mutex);

    if ((bindings = switch_core_hash_find(listener->fetch_bindings, section))) {
        /* loop over all the entries of this sub-hash and send the fetch request to each */
        for (binding = switch_hash_first(NULL, bindings); binding; binding = switch_hash_next(binding)) {
            erlang_pid *pid;
            const void *key;
            void *value;
			
            switch_hash_this(binding, &key, NULL, &value);
            pid = (erlang_pid*) value;

            switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_INFO, "Sending erlang (%s) <%d.%d.%d> fetch request %s: %s / %s / %s = %s\n"
							  ,listener->peer_nodename, pid->creation, pid->num, pid->serial, uuid_str, section, tag_name, key_name, key_value);

            /* TODO: not sure if you can "reuse" the ebuf after sending... */
            ei_helper_send(listener, pid, &ebuf);
		}
	}

	ei_x_free(&ebuf);

    return SWITCH_STATUS_SUCCESS;
}

switch_status_t add_fetch_binding(listener_t *listener, char *section, erlang_pid *from) {
    switch_hash_t *bindings;
    erlang_pid *pid;
    switch_status_t status = SWITCH_STATUS_SUCCESS;
    char key[MAX_PID_CHARS];

    switch_snprintf(key, sizeof(key), "<%d.%d.%d>", from->creation, from->num, from->serial);
    pid = malloc(sizeof(erlang_pid));
    memcpy(pid, from, sizeof(erlang_pid));

    if ((bindings = switch_core_hash_find(listener->fetch_bindings, section))) {
        /* if the fetch_bindings hash has a hash as the value already just insert the pid into the sub-hash */
        switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_NOTICE, "Creating erlang (%s) <%d.%d.%d> fetch binding for %s\n",
                listener->peer_nodename, pid->creation, pid->num, pid->serial, section);
        status = switch_core_hash_insert(bindings, key, pid);
    } else {
        /* if the fetch_bindings hash doesnt have a value for this fetch type, create a new hash with the pid then store the new hash in fetch_bindings */
        switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_NOTICE, "Creating erlang (%s) <%d.%d.%d> intial fetch binding for %s\n",
                listener->peer_nodename, pid->creation, pid->num, pid->serial, section);
        switch_core_hash_init(&bindings, listener->pool);
        switch_core_hash_insert(bindings, key, pid);
        status = switch_core_hash_insert(listener->fetch_bindings, section, bindings);
    }

    ei_link(listener, ei_self(listener->ec), from);
    
    return status;
}

switch_status_t remove_pid_from_fetch_binding(listener_t *listener, char *section, erlang_pid *from) {
    switch_hash_t *bindings;

    if ((bindings = switch_core_hash_find(listener->fetch_bindings, section))) {
        erlang_pid *pid;
        char remove_key[MAX_PID_CHARS];

        /* if fetch_bindings has an hash value for this section type remove pid from the sub-hash */
        switch_snprintf(remove_key, sizeof(remove_key), "<%d.%d.%d>", from->creation, from->num, from->serial);

        if ((pid = (erlang_pid *)switch_core_hash_find(bindings, remove_key))) {
            switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG, "Removed erlang (%s) <%d.%d.%d> binding for %s\n", listener->peer_nodename, pid->creation, pid->num, pid->serial, section);
            switch_core_hash_delete(bindings, remove_key);
            switch_safe_free(pid);
        }
    }

    return SWITCH_STATUS_SUCCESS;
}

switch_status_t remove_pid_from_fetch_bindings(listener_t *listener, erlang_pid *from) {
    switch_hash_index_t *sections;
    switch_hash_t *bindings;
    char remove_key[MAX_PID_CHARS];

    switch_snprintf(remove_key, sizeof(remove_key), "<%d.%d.%d>", from->creation, from->num, from->serial);

    /* loop over all fetch_bindings removing the pid from the hash found as the value */
    for (sections = switch_hash_first(NULL, listener->fetch_bindings); sections; sections = switch_hash_next(sections)) {
        erlang_pid *pid;
        const void *key;
        void *value;

        switch_hash_this(sections, &key, NULL, &value);
        bindings = (switch_hash_t*) value;

        if ((pid = (erlang_pid *) switch_core_hash_find(bindings, remove_key))) {
            switch_core_hash_delete(bindings, remove_key);
            switch_safe_free(pid);
        }
    }

    switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG, "Removed erlang (%s) <%d.%d.%d> fetch bindings\n", listener->peer_nodename, from->creation, from->num, from->serial);

    return SWITCH_STATUS_SUCCESS;
}

switch_status_t flush_fetch_bindings(listener_t *listener) {
    switch_hash_index_t *sections;
    switch_hash_t *bindings;

    /* loop over all fetch_bindings removing all hashes */
    for (sections = switch_hash_first(NULL, listener->fetch_bindings); sections; sections = switch_hash_next(sections)) {
        const void *key;
        void *value;

        switch_hash_this(sections, &key, NULL, &value);
        bindings = (switch_hash_t*) value;

        /* free all the erlang_pids in this hash, then destroy the hash itself */
        switch_core_hash_delete_multi(bindings, bindings_pid_cleanup_callback, NULL);    
        switch_core_hash_destroy(&bindings);
    }
    
    /* delete all the elements in the fetch_bindings hash since they they have been destroyed */
    switch_core_hash_delete_multi(listener->fetch_bindings, NULL, NULL);

    switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG, "Flushed all erlang fetch bindings for node %s\n", listener->peer_nodename);

    return SWITCH_STATUS_SUCCESS;
}






















switch_status_t has_session_bindings(listener_t *listener, char *uuid_str) {
    /* just check if the hash has an entry for this uuid, dont really care */
    /* what value/pids/ect are there yet */
    if (switch_core_hash_find(listener->session_bindings, uuid_str)) {
        return SWITCH_STATUS_FOUND;
    } else {
        return SWITCH_STATUS_NOTFOUND;
    }
}

switch_status_t add_session_binding(listener_t *listener, char *uuid_str, erlang_pid *from) {
    switch_hash_t *bindings;
    erlang_pid *pid;
    switch_status_t status = SWITCH_STATUS_SUCCESS;
    char key[MAX_PID_CHARS];

    switch_snprintf(key, sizeof(key), "<%d.%d.%d>", from->creation, from->num, from->serial);
    pid = malloc(sizeof(erlang_pid));
    memcpy(pid, from, sizeof(erlang_pid));

    if ((bindings = switch_core_hash_find(listener->session_bindings, uuid_str))) {
        /* if the session_bindings hash has a hash as the value already just insert the pid into the sub-hash */
        switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_NOTICE, "Creating erlang (%s) <%d.%d.%d> session binding for %s\n",
                listener->peer_nodename, pid->creation, pid->num, pid->serial, uuid_str);
        status = switch_core_hash_insert(bindings, key, pid);
    } else {
        /* if the session_bindings hash doesnt have a value for this session, create a new hash with the pid then store the new hash in session_bindings */
        switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_NOTICE, "Creating erlang (%s) <%d.%d.%d> intial session binding for %s\n",
                listener->peer_nodename, pid->creation, pid->num, pid->serial, uuid_str);
        switch_core_hash_init(&bindings, listener->pool);
        switch_core_hash_insert(bindings, key, pid);
        status = switch_core_hash_insert(listener->session_bindings, uuid_str, bindings);
    }

    ei_link(listener, ei_self(listener->ec), from);
    
    return status;
}

switch_status_t remove_pid_from_session_binding(listener_t *listener, char *uuid_str, erlang_pid *from) {
    switch_hash_t *bindings;

    if ((bindings = switch_core_hash_find(listener->session_bindings, uuid_str))) {
        erlang_pid *pid;
        char remove_key[MAX_PID_CHARS];

        /* if session_bindings has an hash value for this uuid_str type remove pid from the sub-hash */
        switch_snprintf(remove_key, sizeof(remove_key), "<%d.%d.%d>", from->creation, from->num, from->serial);

        if ((pid = (erlang_pid *)switch_core_hash_find(bindings, remove_key))) {
            switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG, "Removed erlang (%s) <%d.%d.%d> binding for %s\n", listener->peer_nodename, pid->creation, pid->num, pid->serial, uuid_str);
            switch_core_hash_delete(bindings, remove_key);
            switch_safe_free(pid);
        }
    }

    return SWITCH_STATUS_SUCCESS;
}

switch_status_t remove_pid_from_session_bindings(listener_t *listener, erlang_pid *from) {
    switch_hash_index_t *uuid_strs;
    switch_hash_t *bindings;
    char remove_key[MAX_PID_CHARS];

    switch_snprintf(remove_key, sizeof(remove_key), "<%d.%d.%d>", from->creation, from->num, from->serial);

    /* loop over all session_bindings removing the pid from the hash found as the value */
    for (uuid_strs = switch_hash_first(NULL, listener->session_bindings); uuid_strs; uuid_strs = switch_hash_next(uuid_strs)) {
        erlang_pid *pid;
        const void *key;
        void *value;

        switch_hash_this(uuid_strs, &key, NULL, &value);
        bindings = (switch_hash_t*) value;

        if ((pid = (erlang_pid *) switch_core_hash_find(bindings, remove_key))) {
            switch_core_hash_delete(bindings, remove_key);
            switch_safe_free(pid);
        }
    }

    switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG, "Removed erlang (%s) <%d.%d.%d> session bindings\n", listener->peer_nodename, from->creation, from->num, from->serial);

    return SWITCH_STATUS_SUCCESS;
}

switch_status_t flush_session_bindings(listener_t *listener) {
    switch_hash_index_t *uuid_strs;
    switch_hash_t *bindings;

    /* loop over all session_bindings removing all hashes */
    for (uuid_strs = switch_hash_first(NULL, listener->session_bindings); uuid_strs; uuid_strs = switch_hash_next(uuid_strs)) {
        const void *key;
        void *value;

        switch_hash_this(uuid_strs, &key, NULL, &value);
        bindings = (switch_hash_t*) value;

        /* free all the erlang_pids in this hash, then destroy the hash itself */
        switch_core_hash_delete_multi(bindings, bindings_pid_cleanup_callback, NULL);    
        switch_core_hash_destroy(&bindings);
    }
    
    /* delete all the elements in the session_bindings hash since they they have been destroyed */
    switch_core_hash_delete_multi(listener->session_bindings, NULL, NULL);

    switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG, "Flushed all erlang session bindings for node %s\n", listener->peer_nodename);

    return SWITCH_STATUS_SUCCESS;
}


























switch_status_t has_event_bindings(listener_t *listener, switch_event_t *event) {
    const char *event_name;

    event_name = switch_event_get_header(event, "Event-Name");

    /* just check if the hash has an entry for this event type, dont really care */
    /* what value/pids/ect are there yet */
    if (switch_core_hash_find(listener->event_bindings, event_name)) {
        return SWITCH_STATUS_FOUND;
    } else {
        return SWITCH_STATUS_NOTFOUND;
    }
}

switch_status_t send_event_to_bindings(listener_t *listener, switch_event_t *event) {
    switch_hash_t *bindings;
    switch_hash_index_t *binding;
    const char *event_name;

    ei_x_buff ebuf;
    ei_x_new_with_version(&ebuf);

    ei_encode_switch_event(&ebuf, event);

    event_name = switch_event_get_header(event, "Event-Name");

    /* TODO: add appropriate locks... */
    if ((bindings = switch_core_hash_find(listener->event_bindings, event_name))) {
        /* loop over all the entries of this sub-hash and send the event to each */
        for (binding = switch_hash_first(NULL, bindings); binding; binding = switch_hash_next(binding)) {
            erlang_pid *pid;
            const void *key;
            void *value;

            switch_hash_this(binding, &key, NULL, &value);
            pid = (erlang_pid*) value;

            switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG, "Send erlang (%s) <%d.%d.%d> event %s\n", listener->peer_nodename, pid->creation, pid->num, pid->serial, event_name);

            /* TODO: not sure if you can "reuse" the ebuf after sending... */
            ei_helper_send(listener, pid, &ebuf);
        }
    }

    ei_x_free(&ebuf);

    return SWITCH_STATUS_SUCCESS;
}

switch_status_t add_event_binding(listener_t *listener, switch_event_types_t *type, erlang_pid *from) {
    switch_hash_t *bindings;
    erlang_pid *pid;
    switch_status_t status = SWITCH_STATUS_SUCCESS;
    const char *event;
    char key[MAX_PID_CHARS];

    /* TODO: contemplate the locks, luckly switch_core_hash* has locked version of each so if we */
    /*	make it work before mod_kazoo.c handle_event or fs_to_erl_loop step on our toes then */
    /* 	all we have to do is create the appropriate locks... */

    switch_snprintf(key, sizeof(key), "<%d.%d.%d>", from->creation, from->num, from->serial);
    event = switch_event_name(*type);
    pid = malloc(sizeof(erlang_pid));
    memcpy(pid, from, sizeof(erlang_pid));

    if ((bindings = switch_core_hash_find(listener->event_bindings, event))) {
        /* if the event_bindings hash has a hash as the value already just insert the pid into the sub-hash */
        switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_NOTICE, "Creating erlang (%s) <%d.%d.%d> event binding for %s\n",
                listener->peer_nodename, pid->creation, pid->num, pid->serial, event);
        status = switch_core_hash_insert(bindings, key, pid);
    } else {
        /* if the event_bindings hash doesnt have a value for this event type, create a new hash with the pid then store the new hash in event_bindings */
        switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_NOTICE, "Creating erlang (%s) <%d.%d.%d> intial event binding for %s\n",
                listener->peer_nodename, pid->creation, pid->num, pid->serial, event);
        switch_core_hash_init(&bindings, listener->pool);
        switch_core_hash_insert(bindings, key, pid);
        status = switch_core_hash_insert(listener->event_bindings, event, bindings);
    }

    ei_link(listener, ei_self(listener->ec), from);
    
    return status;
}

switch_status_t remove_pid_from_event_binding(listener_t *listener, switch_event_types_t *type, erlang_pid *from) {
    switch_hash_t *bindings;
    const char *event;

    event = switch_event_name(*type);

    if ((bindings = switch_core_hash_find(listener->event_bindings, event))) {
        erlang_pid *pid;
        char remove_key[MAX_PID_CHARS];

        /* if event_bindings has an hash value for this event type remove pid from the sub-hash */
        switch_snprintf(remove_key, sizeof(remove_key), "<%d.%d.%d>", from->creation, from->num, from->serial);

        if ((pid = (erlang_pid *)switch_core_hash_find(bindings, remove_key))) {
            switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG, "Removed erlang (%s) <%d.%d.%d> binding for %s\n", listener->peer_nodename, pid->creation, pid->num, pid->serial, event);
            switch_core_hash_delete(bindings, remove_key);
            switch_safe_free(pid);
        }
    }

    return SWITCH_STATUS_SUCCESS;
}

switch_status_t remove_pid_from_event_bindings(listener_t *listener, erlang_pid *from) {
    switch_hash_index_t *events;
    switch_hash_t *bindings;
    char remove_key[MAX_PID_CHARS];

    switch_snprintf(remove_key, sizeof(remove_key), "<%d.%d.%d>", from->creation, from->num, from->serial);

    /* loop over all event_bindings removing the pid from the hash found as the value */
    for (events = switch_hash_first(NULL, listener->event_bindings); events; events = switch_hash_next(events)) {
        erlang_pid *pid;
        const void *key;
        void *value;

        switch_hash_this(events, &key, NULL, &value);
        bindings = (switch_hash_t*) value;

        if ((pid = (erlang_pid *) switch_core_hash_find(bindings, remove_key))) {
            switch_core_hash_delete(bindings, remove_key);
            switch_safe_free(pid);
        }
    }

    switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG, "Removed erlang (%s) <%d.%d.%d> event bindings\n", listener->peer_nodename, from->creation, from->num, from->serial);

    return SWITCH_STATUS_SUCCESS;
}

switch_status_t flush_event_bindings(listener_t *listener) {
    switch_hash_index_t *events;
    switch_hash_t *bindings;

    /* loop over all event_bindings removing all hashes */
    for (events = switch_hash_first(NULL, listener->event_bindings); events; events = switch_hash_next(events)) {
        const void *key;
        void *value;

        switch_hash_this(events, &key, NULL, &value);
        bindings = (switch_hash_t*) value;

        /* free all the erlang_pids in this hash, then destroy the hash itself */
        switch_core_hash_delete_multi(bindings, bindings_pid_cleanup_callback, NULL);    
        switch_core_hash_destroy(&bindings);
    }
    
    /* delete all the elements in the event_bindings hash since they they have been destroyed */
    switch_core_hash_delete_multi(listener->event_bindings, NULL, NULL);

    switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG, "Flushed all erlang event bindings for node %s\n", listener->peer_nodename);

    return SWITCH_STATUS_SUCCESS;
}

switch_status_t list_event_bindings(listener_t *listener) {
    switch_hash_index_t *events, *binding;
    switch_hash_t *bindings;

    for (events = switch_hash_first(NULL, listener->event_bindings); events; events = switch_hash_next(events)) {        
        const void *key1;
        void *value1;        

        switch_hash_this(events, &key1, NULL, &value1);
        bindings = (switch_hash_t*) value1;

        for (binding = switch_hash_first(NULL, bindings); binding; binding = switch_hash_next(binding)) {
            erlang_pid *pid;
            const void *key2;
            void *value2;

            switch_hash_this(binding, &key2, NULL, &value2);
            pid = (erlang_pid*) value2;

            switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_NOTICE, "Erlang (%s) <%d.%d.%d> is bound to %s\n", listener->peer_nodename, pid->creation, pid->num, pid->serial, (char *) key1);
        }
    }

    return SWITCH_STATUS_SUCCESS;
}

switch_status_t flush_all_bindings(listener_t *listener) {
    flush_event_bindings(listener);
    flush_fetch_bindings(listener);
    
    return SWITCH_STATUS_SUCCESS;
}

switch_status_t remove_pid_from_all_bindings(listener_t *listener, erlang_pid *from) {
    remove_pid_from_event_bindings(listener, from);
    remove_pid_from_fetch_bindings(listener, from);

    return SWITCH_STATUS_SUCCESS;
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

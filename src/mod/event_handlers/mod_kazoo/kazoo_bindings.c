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

switch_status_t remove_pid_from_all_bindings(listener_t *listener, erlang_pid *from)
{
	remove_pid_from_event_bindings(listener, from);

	return SWITCH_STATUS_SUCCESS;
}

switch_status_t has_event_bindings(listener_t *listener, switch_event_t *event)
{
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

switch_status_t send_event_to_bindings(listener_t *listener, switch_event_t *event)
{
	switch_hash_t *bindings;
	switch_hash_index_t *binding;
	const char *event_name;
    
	ei_x_buff ebuf;
	ei_x_new_with_version(&ebuf);

	ei_encode_switch_event(&ebuf, event, &prefs);
    
	event_name = switch_event_get_header(event, "Event-Name");

	/* TODO: add appropriate locks... */
	if ((bindings = switch_core_hash_find(listener->event_bindings, event_name))) {
		/* loop over all the entries of this sub-hash and send the event to each */
		for (binding = switch_hash_first(NULL, bindings); binding; binding = switch_hash_next(binding)) {
			erlang_pid *pid;
			const void *key;
			void *value;
            
			switch_hash_this(binding, &key, NULL, &value);
			pid = (erlang_pid*)value;

			switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG, "Send erlang (%s) <%d.%d.%d> event %s\n", listener->peer_nodename, pid->creation, pid->num, pid->serial, event_name);

			/* TODO: not sure if you can "reuse" the ebuf after sending... */
			ei_helper_send(listener, pid, ebuf.buff, ebuf.index);
		}
	}
    
    ei_x_free(&ebuf);

	return SWITCH_STATUS_SUCCESS;
}

switch_status_t add_event_binding(listener_t *listener, switch_event_types_t *type, erlang_pid *from)
{
	switch_hash_t *bindings;
	erlang_pid *pid;
	switch_status_t status = SWITCH_STATUS_SUCCESS;
	const char *key;
	const char *event;

	/* TODO: contemplate the locks, luckly switch_core_hash* has locked version of each so if we */
	/*	make it work before mod_kazoo.c handle_event or fs_to_erl_loop step on our toes then */
	/* 	all we have to do is create the appropriate locks... */

	key = switch_core_sprintf(listener->pool, "<%d.%d.%d>", from->creation, from->num, from->serial);
	event = switch_core_strdup(listener->pool, switch_event_name(*type));
	pid = switch_core_alloc(listener->pool, sizeof(erlang_pid));
	memcpy(pid, from, sizeof(erlang_pid));

	if ((bindings = switch_core_hash_find(listener->event_bindings, event))) {
		/* if the event_bindings hash has a hash as the value already just insert the pid into the sub-hash */
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_NOTICE, "Creating erlang (%s) <%d.%d.%d> event binding for %s\n",
																	listener->peer_nodename,  pid->creation, pid->num, pid->serial, event);
		status = switch_core_hash_insert(bindings, key, pid);
	} else {
		/* if the event_bindings hash doesnt have a value for this event type, create a new hash with the pid then store the new hash in event_bindings */
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_NOTICE, "Creating erlang (%s) <%d.%d.%d> intial event binding for %s\n",
																	listener->peer_nodename,  pid->creation, pid->num, pid->serial, event);
		switch_core_hash_init(&bindings, listener->pool);
		switch_core_hash_insert(bindings, key, pid);
		status = switch_core_hash_insert(listener->event_bindings, event, bindings);
	}

	ei_link(listener, ei_self(listener->ec), from);
	return status;
}

switch_status_t remove_pid_from_event_binding(listener_t *listener, switch_event_types_t *type, erlang_pid *from)
{
    switch_hash_t *bindings;
    const char *event;

	event = switch_core_strdup(listener->pool, switch_event_name(*type));
	
	/* TODO: sort of locks!!! */

	if ((bindings = switch_core_hash_find(listener->event_bindings, event))) {
	    const char *remove_key;

		/* if event_bindings has an hash value for this event type remove pid from the sub-hash */
		remove_key = switch_core_sprintf(listener->pool, "<%d.%d.%d>", from->creation, from->num, from->serial);

		switch_core_hash_delete(bindings, remove_key);

		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG, "Removed erlang (%s) <%d.%d.%d> binding for %s\n", listener->peer_nodename, from->creation, from->num, from->serial, event);
	}

	return SWITCH_STATUS_SUCCESS;
}

switch_status_t remove_pid_from_event_bindings(listener_t *listener, erlang_pid *from)
{
	switch_hash_index_t *events;
	switch_hash_t *bindings;
	const char *remove_key;

	remove_key = switch_core_sprintf(listener->pool, "<%d.%d.%d>", from->creation, from->num, from->serial);

	/* TODO: sort of locks!!! */

	/* loop over all event_bindings removing the pid from the hash found as the value */
	for (events = switch_hash_first(NULL, listener->event_bindings); events; events = switch_hash_next(events)) {
	    const void *key;
	    void *value;

        switch_hash_this(events, &key, NULL, &value);
        bindings = (switch_hash_t*)value;

		switch_core_hash_delete(bindings, remove_key);
    }
		
	switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG, "Removed erlang (%s) <%d.%d.%d> event bindings\n", listener->peer_nodename,  from->creation, from->num, from->serial);

	return SWITCH_STATUS_SUCCESS;
}

switch_status_t list_event_bindings(listener_t *listener)
{
	switch_hash_index_t *events, *binding;
	switch_hash_t *bindings;
    erlang_pid *pid;

	for (events = switch_hash_first(NULL, listener->event_bindings); events; events = switch_hash_next(events)) {
	    const void *key1;
	    void *value1;

		switch_hash_this(events, &key1, NULL, &value1);
		bindings = (switch_hash_t*)value1;

		for (binding = switch_hash_first(NULL, bindings); binding; binding = switch_hash_next(binding)) {
		    const void *key2;
		    void *value2;

			switch_hash_this(binding, &key2, NULL, &value2);
			pid = (erlang_pid*)value2;

			switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_NOTICE, "Erlang (%s) <%d.%d.%d> is bound to %s\n ", listener->peer_nodename, pid->creation, pid->num, pid->serial, (char *)key1);
		}
	}

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

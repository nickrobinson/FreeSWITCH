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

switch_status_t add_event_binding(listener_t *listener, switch_event_types_t *type, erlang_pid *from) {
	switch_hash_t *bindings;
	erlang_pid *pid;
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
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_NOTICE, "Adding pid %s to existing event bindings for %s\n", key, event);
		return switch_core_hash_insert(bindings, key, pid);
	} else {
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_NOTICE, "Creating new event bindings for %s with pid %s\n", event, key);
		switch_core_hash_init(&bindings, listener->pool);
		switch_core_hash_insert(bindings, key, pid);
		return switch_core_hash_insert(listener->event_bindings, event, bindings);
	}
}

switch_status_t rm_event_pid(listener_t *listener, erlang_pid *from) {
	switch_hash_index_t *events;
	switch_hash_t *bindings;
	const char *remove_key;

	remove_key = switch_core_sprintf(listener->pool, "<%d.%d.%d>", from->creation, from->num, from->serial);

	for (events = switch_hash_first(NULL, listener->event_bindings); events; events = switch_hash_next(events)) {
	    const void *key;
	    void *value;

        switch_hash_this(events, &key, NULL, &value);
        bindings = (switch_hash_t*)value;

		switch_core_hash_delete(bindings, remove_key);
    }

	return SWITCH_STATUS_SUCCESS;
}

switch_status_t list_event_bindings(listener_t *listener) {
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
			switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_NOTICE, "Found binding %s for %s (<%d.%d.%d>)\n", (char *)key1, (char *)key2, pid->creation, pid->num, pid->serial);
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

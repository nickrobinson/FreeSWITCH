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
 * Karl Anderson <karl@2600hz.com>
 * Darren Schreiber <darren@2600hz.com>
 *
 *
 * kazoo_event_bindings.c -- Event Publisher
 *
 */
#include <switch.h>
#include <switch_apr.h>
#include <ei.h>
#include "mod_kazoo.h"

static void *SWITCH_THREAD_FUNC event_bindings_loop(switch_thread_t *thread, void *obj) {
    ei_event_bindings_t *event_bindings = (ei_event_bindings_t *) obj;
	switch_sockaddr_t *sa;
	uint16_t port;
    char ipbuf[25];
    const char *ip_addr;

	switch_atomic_inc(&globals.threads);

	switch_assert(event_bindings != NULL);

	switch_socket_addr_get(&sa, SWITCH_FALSE, event_bindings->acceptor);

	port = switch_sockaddr_get_port(sa);
    ip_addr = switch_get_addr(ipbuf, sizeof (ipbuf), sa);

	switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG, "Starting erlang event stream %p on %s:%u\n", (void *)event_bindings, ip_addr, port);

	/* TODO: point to ei_node? */
	while (switch_test_flag(event_bindings, LFLAG_RUNNING) && switch_test_flag(&globals, LFLAG_RUNNING)) {
		const switch_pollfd_t *fds;
		int32_t numfds;
		void *pop;

		if (switch_pollset_poll(event_bindings->pollset, 0, &numfds, &fds) == SWITCH_STATUS_SUCCESS) {
			for (int32_t i = 0; i < numfds; i++) {
				switch_socket_t *newsocket;

				if (switch_socket_accept(&newsocket, event_bindings->acceptor, event_bindings->pool) == SWITCH_STATUS_SUCCESS) {
					switch_sockaddr_t *sa;
					uint16_t port;
					char ipbuf[25];
					const char *ip_addr;

					/*
                    if (switch_socket_opt_set(newsocket, SWITCH_SO_NONBLOCK, TRUE)) {
                        switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_WARNING, "Couldn't set socket as non-blocking\n");
                    }

                    if (switch_socket_opt_set(newsocket, SWITCH_SO_TCP_NODELAY, 1)) {
                        switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_WARNING, "Couldn't disable Nagle.\n");
                    }
					*/

					switch_socket_addr_get(&sa, SWITCH_TRUE, newsocket);
					
					port = switch_sockaddr_get_port(sa);
					ip_addr = switch_get_addr(ipbuf, sizeof (ipbuf), sa);
					
					switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG, "Erlang event stream client %s:%u\n", ip_addr, port);

					close_socket(&event_bindings->socket);

					event_bindings->socket = newsocket;
				}
			}
		}

		if (switch_queue_trypop(event_bindings->queue, &pop) == SWITCH_STATUS_SUCCESS) {
			switch_event_t *event = (switch_event_t *) pop;

			/* if there was an event waiting in our queue send it to */
			/* any erlang processes bound its type */
			switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_INFO, "POP'D EVENT\n");

 			if (event_bindings->socket) {
				ei_x_buff ebuf;

				ei_x_new_with_version(&ebuf);

				ei_x_encode_tuple_header(&ebuf, 2);
				ei_x_encode_atom(&ebuf, "event");
				ei_encode_switch_event(&ebuf, event);

				switch_socket_send(event_bindings->socket, ebuf.buff, (switch_size_t *) &ebuf.index);

				ei_x_free(&ebuf);
			}

			switch_event_destroy(&event);
		}

		switch_cond_next();
	}

	switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG, "Shutting down erlang event stream %p\n", (void *)event_bindings);

	switch_atomic_dec(&globals.threads);

	return NULL;
}

static void launch_event_bindings(ei_event_bindings_t *event_bindings) {
	switch_thread_t *thread;
	switch_threadattr_t *thd_attr = NULL;
  
	switch_threadattr_create(&thd_attr, event_bindings->pool);
	switch_threadattr_detach_set(thd_attr, 1);
	switch_threadattr_stacksize_set(thd_attr, SWITCH_THREAD_STACKSIZE);
	switch_thread_create(&thread, thd_attr, event_bindings_loop, event_bindings, event_bindings->pool);
}

static void event_handler(switch_event_t *event) {
	switch_event_t *clone = NULL;
	ei_event_bindings_t *event_bindings = (ei_event_bindings_t *) event->bind_user_data;

	/* TODO: point to ei_node? */
	if (!switch_test_flag(event_bindings, LFLAG_RUNNING) || !switch_test_flag(&globals, LFLAG_RUNNING)) {
		return;
	}

	if (switch_event_dup(&clone, event) == SWITCH_STATUS_SUCCESS) {
		if (switch_queue_trypush(event_bindings->queue, clone) != SWITCH_STATUS_SUCCESS) {
			/* if we couldn't place the cloned event into the listeners */
			/* event queue make sure we destroy it, real good like */
			switch_event_destroy(&clone);
		}
	} else {
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "Memory error: Have a good trip? See you next fall!\n");
	}
}

static void log_event_bindings(ei_event_bindings_t *event_bindings) {
	ei_event_binding_t *event_binding;

	event_binding = event_bindings->binding;
	while(event_binding != NULL) {
		if(event_binding->type == SWITCH_EVENT_CUSTOM) {
			switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_INFO, "EVENT(%s): CUSTOM %s\n", event_binding->id, event_binding->subclass_name);
		} else {
			switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_INFO, "EVENT(%s): %s\n", event_binding->id, switch_event_name(event_binding->type));
		}
		event_binding = event_binding->next;
	}
}

static switch_status_t add_event_binding(ei_event_bindings_t *event_bindings, switch_event_types_t event_type, const char *subclass_name) {
	ei_event_binding_t *event_binding;

	event_binding = event_bindings->binding;
	while(event_binding != NULL) {
		if (event_binding->type == SWITCH_EVENT_CUSTOM 
			&& event_binding->subclass_name == subclass_name) {
			return SWITCH_STATUS_SUCCESS;
		} else if (event_binding->type == event_type) {
			return SWITCH_STATUS_SUCCESS;
		}
		event_binding = event_binding->next;
	}

	/* from the erlang node's memory pool, allocate some memory for the structure */
	if (!(event_binding = switch_core_alloc(event_bindings->pool, sizeof (*event_binding)))) {
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "Out of memory: Stan, don't you know the first law of physics? Anything that's fun costs at least eight dollars.\n");
		return SWITCH_STATUS_FALSE;
	}

	event_binding->type = event_type;
	if (subclass_name) {
		event_binding->subclass_name = strdup(subclass_name);
	} else {
		event_binding->subclass_name = NULL;
	}
	event_binding->next = NULL;

	switch_uuid_str(event_binding->id, sizeof(event_binding->id));
	if (switch_event_bind_removable(event_binding->id, event_type, subclass_name, event_handler, event_bindings, &event_binding->node) != SWITCH_STATUS_SUCCESS) {
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "Unable to bind to event!\n");
		return SWITCH_STATUS_GENERR;
	}

	if (!event_bindings->binding) {
		event_bindings->binding = event_binding;
	} else {
		event_binding->next = event_bindings->binding;
		event_bindings->binding = event_binding;
	}

	log_event_bindings(event_bindings);
 
	return SWITCH_STATUS_SUCCESS;
}

static switch_status_t remove_event_binding(ei_event_bindings_t *event_bindings, switch_event_types_t event_type, const char *subclass_name) {
	ei_event_binding_t *event_binding, *prev = NULL;

	if (!event_bindings->binding) {
		return SWITCH_STATUS_SUCCESS;
	}

	event_binding = event_bindings->binding;
	while(event_binding != NULL) {
		if (event_binding->type == SWITCH_EVENT_CUSTOM 
			&& event_binding->subclass_name == subclass_name) {
			break;
		} else if (event_binding->type == event_type) {
			break;
		}

		prev = event_binding;
		event_binding = event_binding->next;
	}

	if (event_binding) {
		switch_event_unbind(&event_binding->node);

		if (!prev) {
			event_bindings->binding = event_binding->next;
		} else {
			prev->next = event_binding->next;
		}
	}

	log_event_bindings(event_bindings);
 
	return SWITCH_STATUS_SUCCESS;
}

switch_status_t add_event_bindings(ei_node_t *ei_node, erlang_pid *from, switch_event_types_t event_type, const char *subclass_name) {
	switch_memory_pool_t *pool = NULL;
	ei_event_bindings_t *event_bindings;

	event_bindings = ei_node->event_bindings;
	while (event_bindings != NULL) {
		if (event_bindings->pid.creation == from->creation
			&& event_bindings->pid.num == from->num
			&& event_bindings->pid.serial == from->serial) {
			/* TODO: create per requesting process */
			return add_event_binding(event_bindings, event_type, subclass_name);
		}
		event_bindings = event_bindings->next;
	}

	/* create memory pool for this erlang node */
	if (switch_core_new_memory_pool(&pool) != SWITCH_STATUS_SUCCESS) {
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "Out of memory: Too bad drinking scotch isn't a paying job or Kenny's dad would be a millionare!\n");
		return SWITCH_STATUS_MEMERR;
	}
	
	/* from the erlang node's memory pool, allocate some memory for the structure */
	if (!(event_bindings = switch_core_alloc(pool, sizeof (*event_bindings)))) {
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "Out of memory: Stan, don't you know the first law of physics? Anything that's fun costs at least eight dollars.\n");
		return SWITCH_STATUS_MEMERR;
	}

	memset(event_bindings, 0, sizeof(*event_bindings));

	event_bindings->binding = NULL;

	event_bindings->pool = pool;

	memcpy(&event_bindings->pid, from, sizeof(erlang_pid));

	switch_queue_create(&event_bindings->queue, MAX_QUEUE_LEN, pool);

    if (!(event_bindings->acceptor = create_socket(pool))) {
        return SWITCH_STATUS_SOCKERR;
    }

	if (switch_socket_opt_set(event_bindings->acceptor, SWITCH_SO_NONBLOCK, TRUE)) {
		return SWITCH_STATUS_SOCKERR;
	}

	//SWITCH_SO_KEEPALIVE

	if (switch_pollset_create(&event_bindings->pollset, 1000, pool, 0) != SWITCH_STATUS_SUCCESS) {
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "Unable to create pollset for node acceptor\n");
		return SWITCH_STATUS_SOCKERR;
	}
	
	switch_socket_create_pollfd(&event_bindings->pollfd, event_bindings->acceptor, SWITCH_POLLIN | SWITCH_POLLERR, NULL, pool);
	if (switch_pollset_add(event_bindings->pollset, event_bindings->pollfd) != SWITCH_STATUS_SUCCESS) {
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "Unable to add acceptor socket pollset fd to pollset\n");
		return SWITCH_STATUS_SOCKERR;
	}

	if (!ei_node->event_bindings) {
		ei_node->event_bindings = event_bindings;
	} else {
		event_bindings->next = ei_node->event_bindings;
		ei_node->event_bindings = event_bindings;
	}

	/* when we start we are running */
	switch_set_flag(event_bindings, LFLAG_RUNNING);

	launch_event_bindings(event_bindings);

	ei_link(ei_node, ei_self(&globals.ei_cnode), from);

	return add_event_binding(event_bindings, event_type, subclass_name);
}

switch_status_t remove_event_bindings(ei_node_t *ei_node, erlang_pid *from, switch_event_types_t event_type, const char *subclass_name) {
	ei_event_bindings_t *event_bindings;

	event_bindings = ei_node->event_bindings;
	while (event_bindings != NULL) {
		if (event_bindings->pid.creation == from->creation
			&& event_bindings->pid.num == from->num
			&& event_bindings->pid.serial == from->serial) {
			return remove_event_binding(event_bindings, event_type, subclass_name);
		}
		event_bindings = event_bindings->next;
	}

	return SWITCH_STATUS_SUCCESS;
}

switch_status_t remove_pid_event_bindings(ei_node_t *ei_node, erlang_pid *from) {
	ei_event_bindings_t *event_bindings, *prev = NULL;

	if (!ei_node->event_bindings) {
		return SWITCH_STATUS_SUCCESS;
	}

	event_bindings = ei_node->event_bindings;
	while(event_bindings != NULL) {
		if (event_bindings->pid.creation == from->creation
			&& event_bindings->pid.num == from->num
			&& event_bindings->pid.serial == from->serial) {
			break;
		}

		prev = event_bindings;
		event_bindings = event_bindings->next;
	}

	if (event_bindings) {
		ei_event_binding_t *event_binding;
		void *pop;

		switch_clear_flag(event_bindings, LFLAG_RUNNING);

		event_binding = event_bindings->binding;
		while(event_binding != NULL) {
			switch_event_unbind(&event_binding->node);
			event_binding = event_binding->next;
		}

		while (switch_queue_trypop(event_bindings->queue, &pop) == SWITCH_STATUS_SUCCESS) {
			switch_event_t *event = (switch_event_t *) pop;
			switch_event_destroy(&event);		
		}

		switch_pollset_remove(event_bindings->pollset, event_bindings->pollfd);
		
		close_socket(&event_bindings->acceptor);
		
		close_socket(&event_bindings->socket);

		switch_core_destroy_memory_pool(&event_bindings->pool);

		if (!prev) {
			ei_node->event_bindings = event_bindings->next;
		} else {
			prev->next = event_bindings->next;
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

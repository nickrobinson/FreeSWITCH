static switch_xml_t fetch_handler(const char *section, const char *tag_name, const char *key_name, const char *key_value, switch_event_t *params, void *user_data) {
	switch_xml_t xml = NULL;
	xml_fetch_msg_t *fetch_msg = NULL;
	switch_uuid_t uuid;
	listener_t *listener = NULL;
	switch_time_t now = 0;

	if (!switch_test_flag(&globals, LFLAG_RUNNING) || !acceptor.ready) {
		return xml;
	}

	now = switch_micro_time_now();

    /* switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_INFO, "Got fetch request: %s / %s / %s = %s\n", section, tag_name, key_name, key_value); */

	switch_thread_rwlock_rdlock(acceptor.listeners_lock);
	for (listener = acceptor.listeners; listener; listener = listener->next) {
		if (has_fetch_bindings(listener, section) == SWITCH_STATUS_FOUND) {
			char *dup_uuid;

			if (!fetch_msg) {
				switch_zmalloc(fetch_msg, sizeof(*fetch_msg));
				switch_mutex_init(&fetch_msg->mutex, SWITCH_MUTEX_DEFAULT, globals.pool);
				switch_thread_cond_create(&fetch_msg->response_available, globals.pool);

				/* Create a unique identifier for this request */
				switch_uuid_get(&uuid);
				switch_uuid_format(fetch_msg->uuid_str, &uuid);

				/* Create a request in our queue for an XML binding. No need to copy memory pointers here, we block until they're used elsewhere and returned */
				fetch_msg->section = section;
				fetch_msg->tag_name = tag_name;
				fetch_msg->key_name = key_name;
				fetch_msg->key_value = key_value;
				fetch_msg->params = params;

				switch_core_hash_insert_wrlock(globals.fetch_resp_hash, fetch_msg->uuid_str, fetch_msg, globals.fetch_resp_lock);

				/* These is an extremely unlikely race condition here.  If the network request and corresponding erlang response arrives */
				/* before we get to switch_thread_cond_timedwait bellow, the response will be thrown away.... but there is no way communication */
				/* between two servers is faster than C code on one.... right?  Ya, I am sure thats right. */
				switch_mutex_lock(fetch_msg->mutex);
			}

			dup_uuid = strdup(fetch_msg->uuid_str);

			if (switch_queue_trypush(listener->fetch_queue, dup_uuid) != SWITCH_STATUS_SUCCESS) {
				switch_safe_free(dup_uuid);
			}
		}
	}
	switch_thread_rwlock_unlock(acceptor.listeners_lock);

	if (fetch_msg) {
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG, "Processing fetch request %s: %s / %s / %s = %s\n", fetch_msg->uuid_str, section, tag_name, key_name, key_value);

		switch_thread_cond_timedwait(fetch_msg->response_available, fetch_msg->mutex, 3000000);

		if (fetch_msg->xml) {
			switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG, "Fetch request %s reply after %dms\n", fetch_msg->uuid_str, (int) (switch_micro_time_now() - now) / 1000);
			xml = fetch_msg->xml;
		} else {
			switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_WARNING, "Fetch request %s timeout after %dms\n", fetch_msg->uuid_str, (int) (switch_micro_time_now() - now) / 1000);
		}

		switch_core_hash_delete_wrlock(globals.fetch_resp_hash, fetch_msg->uuid_str, globals.fetch_resp_lock);

		switch_mutex_unlock(fetch_msg->mutex);

		switch_thread_cond_destroy(fetch_msg->response_available);
		switch_mutex_destroy(fetch_msg->mutex);
		switch_safe_free(fetch_msg);
	}

	return xml;
}




/* initialize the xml fetch response lock */
switch_thread_rwlock_create(&globals.fetch_resp_lock, pool);

/* initialize the xml fetch response hash */
switch_core_hash_init(&globals.fetch_resp_hash, pool);

/* bind to all XML requests */
/*	if (switch_xml_bind_search_function_ret(fetch_handler, SWITCH_XML_SECTION_CONFIG, NULL, &globals.config_fetch_binding) != SWITCH_STATUS_SUCCESS) {
	switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "Could not bind to FreeSWITCH configuration XML\n");
	return SWITCH_STATUS_GENERR;
}

if (switch_xml_bind_search_function_ret(fetch_handler, SWITCH_XML_SECTION_DIRECTORY, NULL, &globals.directory_fetch_binding) != SWITCH_STATUS_SUCCESS) {
	switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "Could not bind to FreeSWITCH directory XML\n");
	return SWITCH_STATUS_GENERR;
}

if (switch_xml_bind_search_function_ret(fetch_handler, SWITCH_XML_SECTION_DIALPLAN, NULL, &globals.dialplan_fetch_binding) != SWITCH_STATUS_SUCCESS) {
	switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "Could not bind to FreeSWITCH dialplan XML\n");
	return SWITCH_STATUS_GENERR;
}

if (switch_xml_bind_search_function_ret(fetch_handler, SWITCH_XML_SECTION_CHATPLAN, NULL, &globals.chatplan_fetch_binding) != SWITCH_STATUS_SUCCESS) {
	switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "Could not bind to FreeSWITCH chatplan XML\n");
	return SWITCH_STATUS_GENERR;
}
*/

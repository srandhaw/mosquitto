/*
Copyright (c) 2010-2019 Roger Light <roger@atchoo.org>

All rights reserved. This program and the accompanying materials
are made available under the terms of the Eclipse Public License v1.0
and Eclipse Distribution License v1.0 which accompany this distribution.
 
The Eclipse Public License is available at
   http://www.eclipse.org/legal/epl-v10.html
and the Eclipse Distribution License is available at
  http://www.eclipse.org/org/documents/edl-v10.php.
 
Contributors:
   Roger Light - initial implementation and documentation.
*/

/* A note on matching topic subscriptions.
 *
 * Topics can be up to 32767 characters in length. The / character is used as a
 * hierarchy delimiter. Messages are published to a particular topic.
 * Clients may subscribe to particular topics directly, but may also use
 * wildcards in subscriptions.  The + and # characters are used as wildcards.
 * The # wildcard can be used at the end of a subscription only, and is a
 * wildcard for the level of hierarchy at which it is placed and all subsequent
 * levels.
 * The + wildcard may be used at any point within the subscription and is a
 * wildcard for only the level of hierarchy at which it is placed.
 * Neither wildcard may be used as part of a substring.
 * Valid:
 * 	a/b/+
 * 	a/+/c
 * 	a/#
 * 	a/b/#
 * 	#
 * 	+/b/c
 * 	+/+/+
 * Invalid:
 *	a/#/c
 *	a+/b/c
 * Valid but non-matching:
 *	a/b
 *	a/+
 *	+/b
 *	b/c/a
 *	a/b/d
 */

#include "config.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "mosquitto_broker_internal.h"
#include "memory_mosq.h"
#include "mqtt_protocol.h"
#include "util_mosq.h"

#include "utlist.h"

#ifdef WITH_CJSON
#  include <cJSON.h>
#endif

static int subs__send(struct mosquitto_db *db, struct mosquitto__subleaf *leaf, const char *topic, int qos, int retain, struct mosquitto_msg_store *stored)
{
	bool client_retain;
	uint16_t mid;
	int client_qos, msg_qos;
	mosquitto_property *properties = NULL;
	int rc2;

	/* Check for ACL topic access. */
	rc2 = mosquitto_acl_check(db, leaf->context, topic, stored->payloadlen, UHPA_ACCESS(stored->payload, stored->payloadlen), stored->qos, stored->retain, MOSQ_ACL_READ);
	if(rc2 == MOSQ_ERR_ACL_DENIED){
		return MOSQ_ERR_SUCCESS;
	}else if(rc2 == MOSQ_ERR_SUCCESS){
		client_qos = leaf->qos;

		if(db->config->upgrade_outgoing_qos){
			msg_qos = client_qos;
		}else{
			if(qos > client_qos){
				msg_qos = client_qos;
			}else{
				msg_qos = qos;
			}
		}
		if(msg_qos){
			mid = mosquitto__mid_generate(leaf->context);
		}else{
			mid = 0;
		}
		if(leaf->retain_as_published){
			client_retain = retain;
		}else{
			client_retain = false;
		}
		if(leaf->identifier){
			mosquitto_property_add_varint(&properties, MQTT_PROP_SUBSCRIPTION_IDENTIFIER, leaf->identifier);
		}
		if(db__message_insert(db, leaf->context, mid, mosq_md_out, msg_qos, client_retain, stored, properties) == 1){
			return 1;
		}
	}else{
		return 1; /* Application error */
	}
	return 0;
}


static int subs__shared_process(struct mosquitto_db *db, struct mosquitto__subhier *hier, const char *topic, int qos, int retain, struct mosquitto_msg_store *stored)
{
	int rc = 0, rc2;
	struct mosquitto__subshared *shared, *shared_tmp;
	struct mosquitto__subleaf *leaf;

	HASH_ITER(hh, hier->shared, shared, shared_tmp){
		leaf = shared->subs;
		rc2 = subs__send(db, leaf, topic, qos, retain, stored);
		/* Remove current from the top, add back to the bottom */
		DL_DELETE(shared->subs, leaf);
		DL_APPEND(shared->subs, leaf);

		if(rc2) rc = 1;
	}

	return rc;
}

static int subs__process(struct mosquitto_db *db, struct mosquitto__subhier *hier, const char *source_id, const char *topic, int qos, int retain, struct mosquitto_msg_store *stored)
{
	int rc = 0;
	int rc2;
	struct mosquitto__subleaf *leaf;

	rc = subs__shared_process(db, hier, topic, qos, retain, stored);

	leaf = hier->subs;
	while(source_id && leaf){
		if(!leaf->context->id || (leaf->no_local && !strcmp(leaf->context->id, source_id))){
			leaf = leaf->next;
			continue;
		}
		rc2 = subs__send(db, leaf, topic, qos, retain, stored);
		if(rc2){
			rc = 1;
		}
		leaf = leaf->next;
	}
	if(hier->subs || hier->shared){
		return rc;
	}else{
		return MOSQ_ERR_NO_SUBSCRIBERS;
	}
}


static int sub__add_leaf(struct mosquitto *context, int qos, uint32_t identifier, int options, struct mosquitto__subleaf **head, struct mosquitto__subleaf **newleaf)
{
	struct mosquitto__subleaf *leaf;

	*newleaf = NULL;
	leaf = *head;

	while(leaf){
		if(leaf->context && leaf->context->id && !strcmp(leaf->context->id, context->id)){
			/* Client making a second subscription to same topic. Only
			 * need to update QoS. Return MOSQ_ERR_SUB_EXISTS to
			 * indicate this to the calling function. */
			leaf->qos = qos;
			leaf->identifier = identifier;
			return MOSQ_ERR_SUB_EXISTS;
		}
		leaf = leaf->next;
	}
	leaf = mosquitto__calloc(1, sizeof(struct mosquitto__subleaf));
	if(!leaf) return MOSQ_ERR_NOMEM;
	leaf->context = context;
	leaf->qos = qos;
	leaf->identifier = identifier;
	leaf->no_local = ((options & MQTT_SUB_OPT_NO_LOCAL) != 0);
	leaf->retain_as_published = ((options & MQTT_SUB_OPT_RETAIN_AS_PUBLISHED) != 0);

	DL_APPEND(*head, leaf);
	*newleaf = leaf;

	return MOSQ_ERR_SUCCESS;
}


static void sub__remove_shared_leaf(struct mosquitto__subhier *subhier, struct mosquitto__subshared *shared, struct mosquitto__subleaf *leaf)
{
	DL_DELETE(shared->subs, leaf);
	if(shared->subs == NULL){
		HASH_DELETE(hh, subhier->shared, shared);
		mosquitto__free(shared->name);
		mosquitto__free(shared);
	}
	mosquitto__free(leaf);
}


static int sub__add_shared(struct mosquitto_db *db, struct mosquitto *context, int qos, uint32_t identifier, int options, struct mosquitto__subhier *subhier, char *sharename)
{
	struct mosquitto__subleaf *newleaf;
	struct mosquitto__subshared *shared = NULL;
	struct mosquitto__subshared_ref **shared_subs;
	struct mosquitto__subshared_ref *shared_ref;
	int i;
	unsigned int slen;
	int rc;

	slen = strlen(sharename);

	HASH_FIND(hh, subhier->shared, sharename, slen, shared);
	if(shared){
		mosquitto__free(sharename);
	}else{
		shared = mosquitto__calloc(1, sizeof(struct mosquitto__subshared));
		if(!shared){
			mosquitto__free(sharename);
			return MOSQ_ERR_NOMEM;
		}
		shared->name = sharename;

		HASH_ADD_KEYPTR(hh, subhier->shared, shared->name, slen, shared);
	}

	rc = sub__add_leaf(context, qos, identifier, options, &shared->subs, &newleaf);
	if(rc > 0){
		if(shared->subs == NULL){
			HASH_DELETE(hh, subhier->shared, shared);
			mosquitto__free(shared->name);
			mosquitto__free(shared);
		}
		return rc;
	}

	if(rc != MOSQ_ERR_SUB_EXISTS){
		shared_ref = mosquitto__calloc(1, sizeof(struct mosquitto__subshared_ref));
		if(!shared_ref){
			sub__remove_shared_leaf(subhier, shared, newleaf);
			return MOSQ_ERR_NOMEM;
		}
		shared_ref->hier = subhier;
		shared_ref->shared = shared;

		for(i=0; i<context->shared_sub_count; i++){
			if(!context->shared_subs[i]){
				context->shared_subs[i] = shared_ref;
				break;
			}
		}
		if(i == context->shared_sub_count){
			shared_subs = mosquitto__realloc(context->shared_subs, sizeof(struct mosquitto__subhier_ref *)*(context->shared_sub_count + 1));
			if(!shared_subs){
				sub__remove_shared_leaf(subhier, shared, newleaf);
				return MOSQ_ERR_NOMEM;
			}
			context->shared_subs = shared_subs;
			context->shared_sub_count++;
			context->shared_subs[context->shared_sub_count-1] = shared_ref;
		}
#ifdef WITH_SYS_TREE
		db->shared_subscription_count++;
#endif
	}

	if(context->protocol == mosq_p_mqtt31 || context->protocol == mosq_p_mqtt5){
		return rc;
	}else{
		/* mqttv311/mqttv5 requires retained messages are resent on
		 * resubscribe. */
		return MOSQ_ERR_SUCCESS;
	}
}


static int sub__add_normal(struct mosquitto_db *db, struct mosquitto *context, int qos, uint32_t identifier, int options, struct mosquitto__subhier *subhier)
{
	struct mosquitto__subleaf *newleaf = NULL;
	struct mosquitto__subhier **subs;
	int i;
	int rc;

	rc = sub__add_leaf(context, qos, identifier, options, &subhier->subs, &newleaf);
	if(rc > 0){
		return rc;
	}

	if(rc != MOSQ_ERR_SUB_EXISTS){
		for(i=0; i<context->sub_count; i++){
			if(!context->subs[i]){
				context->subs[i] = subhier;
				break;
			}
		}
		if(i == context->sub_count){
			subs = mosquitto__realloc(context->subs, sizeof(struct mosquitto__subhier *)*(context->sub_count + 1));
			if(!subs){
				DL_DELETE(subhier->subs, newleaf);
				mosquitto__free(newleaf);
				return MOSQ_ERR_NOMEM;
			}
			context->subs = subs;
			context->sub_count++;
			context->subs[context->sub_count-1] = subhier;
		}
#ifdef WITH_SYS_TREE
		db->subscription_count++;
#endif
	}

	if(context->protocol == mosq_p_mqtt31 || context->protocol == mosq_p_mqtt5){
		return rc;
	}else{
		/* mqttv311/mqttv5 requires retained messages are resent on
		 * resubscribe. */
		return MOSQ_ERR_SUCCESS;
	}
}


static int sub__add_context(struct mosquitto_db *db, struct mosquitto *context, int qos, uint32_t identifier, int options, struct mosquitto__subhier *subhier, struct sub__token *tokens, char *sharename)
{
	struct mosquitto__subhier *branch;

	/* Find leaf node */
	while(tokens){
		HASH_FIND(hh, subhier->children, tokens->topic, tokens->topic_len, branch);
		if(!branch){
			/* Not found */
			branch = sub__add_hier_entry(subhier, &subhier->children, tokens->topic, tokens->topic_len);
			if(!branch) return MOSQ_ERR_NOMEM;
		}
		subhier = branch;
		tokens = tokens ->next;
	}

	/* Add add our context */
	if(context && context->id){
		if(sharename){
			return sub__add_shared(db, context, qos, identifier, options, subhier, sharename);
		}else{
			return sub__add_normal(db, context, qos, identifier, options, subhier);
		}
	}else{
		return MOSQ_ERR_SUCCESS;
	}
}


static int sub__remove_normal(struct mosquitto_db *db, struct mosquitto *context, struct mosquitto__subhier *subhier, uint8_t *reason)
{
	struct mosquitto__subleaf *leaf;
	int i;

	leaf = subhier->subs;
	while(leaf){
		if(leaf->context==context){
#ifdef WITH_SYS_TREE
			db->subscription_count--;
#endif
			DL_DELETE(subhier->subs, leaf);
			mosquitto__free(leaf);

			/* Remove the reference to the sub that the client is keeping.
			 * It would be nice to be able to use the reference directly,
			 * but that would involve keeping a copy of the topic string in
			 * each subleaf. Might be worth considering though. */
			for(i=0; i<context->sub_count; i++){
				if(context->subs[i] == subhier){
					context->subs[i] = NULL;
					break;
				}
			}
			*reason = 0;
			return MOSQ_ERR_SUCCESS;
		}
		leaf = leaf->next;
	}
	return MOSQ_ERR_NO_SUBSCRIBERS;
}


static int sub__remove_shared(struct mosquitto_db *db, struct mosquitto *context, struct mosquitto__subhier *subhier, uint8_t *reason, char *sharename)
{
	struct mosquitto__subshared *shared;
	struct mosquitto__subleaf *leaf;
	int i;

	HASH_FIND(hh, subhier->shared, sharename, strlen(sharename), shared);
	mosquitto__free(sharename);
	if(shared){
		leaf = shared->subs;
		while(leaf){
			if(leaf->context==context){
#ifdef WITH_SYS_TREE
				db->shared_subscription_count--;
#endif
				DL_DELETE(shared->subs, leaf);
				mosquitto__free(leaf);

				/* Remove the reference to the sub that the client is keeping.
				* It would be nice to be able to use the reference directly,
				* but that would involve keeping a copy of the topic string in
				* each subleaf. Might be worth considering though. */
				for(i=0; i<context->shared_sub_count; i++){
					if(context->shared_subs[i]
							&& context->shared_subs[i]->hier == subhier
							&& context->shared_subs[i]->shared == shared){

						mosquitto__free(context->shared_subs[i]);
						context->shared_subs[i] = NULL;
						break;
					}
				}

				if(shared->subs == NULL){
					HASH_DELETE(hh, subhier->shared, shared);
					mosquitto__free(shared->name);
					mosquitto__free(shared);
				}

				*reason = 0;
				return MOSQ_ERR_SUCCESS;
			}
			leaf = leaf->next;
		}
		return MOSQ_ERR_NO_SUBSCRIBERS;
	}else{
		return MOSQ_ERR_NO_SUBSCRIBERS;
	}
}


static int sub__remove_recurse(struct mosquitto_db *db, struct mosquitto *context, struct mosquitto__subhier *subhier, struct sub__token *tokens, uint8_t *reason, char *sharename)
{
	struct mosquitto__subhier *branch;

	if(!tokens){
		if(sharename){
			return sub__remove_shared(db, context, subhier, reason, sharename);
		}else{
			return sub__remove_normal(db, context, subhier, reason);
		}
	}

	HASH_FIND(hh, subhier->children, tokens->topic, tokens->topic_len, branch);
	if(branch){
		sub__remove_recurse(db, context, branch, tokens->next, reason, sharename);
		if(!branch->children && !branch->subs && !branch->shared){
			HASH_DELETE(hh, subhier->children, branch);
			mosquitto__free(branch->topic);
			mosquitto__free(branch);
		}
	}
	return MOSQ_ERR_SUCCESS;
}

static int sub__search(struct mosquitto_db *db, struct mosquitto__subhier *subhier, struct sub__token *tokens, const char *source_id, const char *topic, int qos, int retain, struct mosquitto_msg_store *stored)
{
	/* FIXME - need to take into account source_id if the client is a bridge */
	struct mosquitto__subhier *branch;
	int rc;
	bool have_subscribers = false;

	if(tokens){
		/* Check for literal match */
		HASH_FIND(hh, subhier->children, tokens->topic, tokens->topic_len, branch);

		if(branch){
			rc = sub__search(db, branch, tokens->next, source_id, topic, qos, retain, stored);
			if(rc == MOSQ_ERR_SUCCESS){
				have_subscribers = true;
			}else if(rc != MOSQ_ERR_NO_SUBSCRIBERS){
				return rc;
			}
			if(!tokens->next){
				rc = subs__process(db, branch, source_id, topic, qos, retain, stored);
				if(rc == MOSQ_ERR_SUCCESS){
					have_subscribers = true;
				}else if(rc != MOSQ_ERR_NO_SUBSCRIBERS){
					return rc;
				}
			}
		}

		/* Check for + match */
		HASH_FIND(hh, subhier->children, "+", 1, branch);

		if(branch){
			rc = sub__search(db, branch, tokens->next, source_id, topic, qos, retain, stored);
			if(rc == MOSQ_ERR_SUCCESS){
				have_subscribers = true;
			}else if(rc != MOSQ_ERR_NO_SUBSCRIBERS){
				return rc;
			}
			if(!tokens->next){
				rc = subs__process(db, branch, source_id, topic, qos, retain, stored);
				if(rc == MOSQ_ERR_SUCCESS){
					have_subscribers = true;
				}else if(rc != MOSQ_ERR_NO_SUBSCRIBERS){
					return rc;
				}
			}
		}
	}

	/* Check for # match */
	HASH_FIND(hh, subhier->children, "#", 1, branch);
	if(branch && !branch->children){
		/* The topic matches due to a # wildcard - process the
		 * subscriptions but *don't* return. Although this branch has ended
		 * there may still be other subscriptions to deal with.
		 */
		rc = subs__process(db, branch, source_id, topic, qos, retain, stored);
		if(rc == MOSQ_ERR_SUCCESS){
			have_subscribers = true;
		}else if(rc != MOSQ_ERR_NO_SUBSCRIBERS){
			return rc;
		}
	}

	if(have_subscribers){
		return MOSQ_ERR_SUCCESS;
	}else{
		return MOSQ_ERR_NO_SUBSCRIBERS;
	}
}


struct mosquitto__subhier *sub__add_hier_entry(struct mosquitto__subhier *parent, struct mosquitto__subhier **sibling, const char *topic, size_t len)
{
	struct mosquitto__subhier *child;

	assert(sibling);

	child = mosquitto__calloc(1, sizeof(struct mosquitto__subhier));
	if(!child){
		log__printf(NULL, MOSQ_LOG_ERR, "Error: Out of memory.");
		return NULL;
	}
	child->parent = parent;
	child->topic_len = len;
	child->topic = mosquitto__malloc(len+1);
	if(!child->topic){
		child->topic_len = 0;
		mosquitto__free(child);
		log__printf(NULL, MOSQ_LOG_ERR, "Error: Out of memory.");
		return NULL;
	}else{
		strncpy(child->topic, topic, child->topic_len+1);
	}

	HASH_ADD_KEYPTR(hh, *sibling, child->topic, child->topic_len, child);

	return child;
}


int sub__add(struct mosquitto_db *db, struct mosquitto *context, const char *sub, int qos, uint32_t identifier, int options, struct mosquitto__subhier **root)
{
	int rc = 0;
	struct mosquitto__subhier *subhier;
	struct sub__token *tokens = NULL, *t;
	char *sharename = NULL;

	assert(root);
	assert(*root);
	assert(sub);

	if(sub__topic_tokenise(sub, &tokens)) return 1;

	if(!strcmp(tokens->topic, "$share")){
		if(!tokens->next || !tokens->next->next){
			sub__topic_tokens_free(tokens);
			return MOSQ_ERR_PROTOCOL;
		}
		t = tokens->next;
		mosquitto__free(tokens->topic);
		mosquitto__free(tokens);
		tokens = t;

		sharename = tokens->topic;

		tokens->topic = mosquitto__strdup("");
		if(!tokens->topic){
			tokens->topic = sharename;
			sub__topic_tokens_free(tokens);
			return MOSQ_ERR_PROTOCOL;
		}
		tokens->topic_len = 0;
	}

	HASH_FIND(hh, *root, tokens->topic, tokens->topic_len, subhier);
	if(!subhier){
		subhier = sub__add_hier_entry(NULL, root, tokens->topic, tokens->topic_len);
		if(!subhier){
			sub__topic_tokens_free(tokens);
			log__printf(NULL, MOSQ_LOG_ERR, "Error: Out of memory.");
			return MOSQ_ERR_NOMEM;
		}

	}
	rc = sub__add_context(db, context, qos, identifier, options, subhier, tokens, sharename);

	sub__topic_tokens_free(tokens);

	return rc;
}

int sub__remove(struct mosquitto_db *db, struct mosquitto *context, const char *sub, struct mosquitto__subhier *root, uint8_t *reason)
{
	int rc = 0;
	struct mosquitto__subhier *subhier;
	struct sub__token *tokens = NULL, *t;
	char *sharename = NULL;

	assert(root);
	assert(sub);

	if(sub__topic_tokenise(sub, &tokens)) return 1;

	if(!strcmp(tokens->topic, "$share")){
		if(!tokens->next || !tokens->next->next){
			sub__topic_tokens_free(tokens);
			return MOSQ_ERR_PROTOCOL;
		}
		t = tokens->next;
		mosquitto__free(tokens->topic);
		mosquitto__free(tokens);
		tokens = t;

		sharename = tokens->topic;

		tokens->topic = mosquitto__strdup("");
		if(!tokens->topic){
			tokens->topic = sharename;
			sub__topic_tokens_free(tokens);
			return MOSQ_ERR_PROTOCOL;
		}
		tokens->topic_len = 0;
	}

	HASH_FIND(hh, root, tokens->topic, tokens->topic_len, subhier);
	if(subhier){
		*reason = MQTT_RC_NO_SUBSCRIPTION_EXISTED;
		rc = sub__remove_recurse(db, context, subhier, tokens, reason, sharename);
	}

	sub__topic_tokens_free(tokens);

	return rc;
}

int sub__messages_queue(struct mosquitto_db *db, const char *source_id, const char *topic, int qos, int retain, struct mosquitto_msg_store **stored)
{
	int rc = 0, rc2;
	struct mosquitto__subhier *subhier;
	struct sub__token *tokens = NULL;

	assert(db);
	assert(topic);

#ifdef WITH_CJSON
	// BASH check
	if(topic[0] != '$') {
		//cJSON *root;
		char * bash_tmp = (char *)UHPA_ACCESS((**stored).payload, (**stored).payloadlen);
		//char bash_tmp[] = "{\"ts\": \"1000\"}";
		//log__printf(NULL, MOSQ_LOG_DEBUG, "BASH: sub__messages_queue message is %s and source id is NULL? %d", (char *)UHPA_ACCESS_PAYLOAD(*stored), (*stored)->source_id == NULL);
		log__printf(NULL, MOSQ_LOG_DEBUG, "BASH: sub__mesages_queue topic is %s and message is %s", topic, bash_tmp);
		// bash_tmp = (char *)UHPA_ACCESS((**stored).payload, (**stored).payloadlen);
		// TODO something wrong with accessing UHPA_ACCESS with asan.

		cJSON *monitor_json = cJSON_Parse(bash_tmp);
		if(monitor_json == NULL) {
			const char *error_ptr = cJSON_GetErrorPtr();
			if(error_ptr) {
				log__printf(NULL, MOSQ_LOG_DEBUG, "BASH: cJSON error %s", error_ptr);
			}
		} else {
			log__printf(NULL, MOSQ_LOG_DEBUG, "BASH: cJSON parse completed");
			cJSON *bash_ts = NULL;
			bash_ts = cJSON_GetObjectItemCaseSensitive(monitor_json, "ts");
			if(cJSON_IsNumber(bash_ts)) {
				log__printf(NULL, MOSQ_LOG_DEBUG, "BASH: timestamp value is %u", (uint64_t)bash_ts->valuedouble);
			}
		}
	}
#endif

	if(sub__topic_tokenise(topic, &tokens)) return 1;

	/* Protect this message until we have sent it to all
	clients - this is required because websockets client calls
	db__message_write(), which could remove the message if ref_count==0.
	*/
	db__msg_store_ref_inc(*stored);

	HASH_FIND(hh, db->subs, tokens->topic, tokens->topic_len, subhier);
	if(subhier){
		rc = sub__search(db, subhier, tokens, source_id, topic, qos, retain, *stored);
	}

	if(retain){
		rc2 = retain__store(db, topic, *stored, tokens);
		if(rc2){
			sub__topic_tokens_free(tokens);
			db__msg_store_ref_dec(db, stored);
			return rc2;
		}
	}
	sub__topic_tokens_free(tokens);
	/* Remove our reference and free if needed. */
	db__msg_store_ref_dec(db, stored);

	return rc;
}


/* Remove a subhier element, and return its parent if that needs freeing as well. */
static struct mosquitto__subhier *tmp_remove_subs(struct mosquitto__subhier *sub)
{
	struct mosquitto__subhier *parent;

	if(!sub || !sub->parent){
		return NULL;
	}

	if(sub->children || sub->subs){
		return NULL;
	}

	parent = sub->parent;
	HASH_DELETE(hh, parent->children, sub);
	mosquitto__free(sub->topic);
	mosquitto__free(sub);

	if(parent->subs == NULL
			&& parent->children == NULL
			&& parent->shared == NULL
			&& parent->parent){

		return parent;
	}else{
		return NULL;
	}
}


static int sub__clean_session_shared(struct mosquitto_db *db, struct mosquitto *context)
{
	int i;
	struct mosquitto__subleaf *leaf;
	struct mosquitto__subhier *hier;

	for(i=0; i<context->shared_sub_count; i++){
		if(context->shared_subs[i] == NULL){
			continue;
		}
		leaf = context->shared_subs[i]->shared->subs;
		while(leaf){
			if(leaf->context==context){
#ifdef WITH_SYS_TREE
				db->shared_subscription_count--;
#endif
				sub__remove_shared_leaf(context->shared_subs[i]->hier, context->shared_subs[i]->shared, leaf);
				break;
			}
			leaf = leaf->next;
		}
		if(context->shared_subs[i]->hier->subs == NULL
				&& context->shared_subs[i]->hier->children == NULL
				&& context->shared_subs[i]->hier->shared == NULL
				&& context->shared_subs[i]->hier->parent){

			hier = context->shared_subs[i]->hier;
			context->shared_subs[i]->hier = NULL;
			do{
				hier = tmp_remove_subs(hier);
			}while(hier);
		}
		mosquitto__free(context->shared_subs[i]);
	}
	mosquitto__free(context->shared_subs);
	context->shared_subs = NULL;
	context->shared_sub_count = 0;

	return MOSQ_ERR_SUCCESS;
}

/* Remove all subscriptions for a client.
 */
int sub__clean_session(struct mosquitto_db *db, struct mosquitto *context)
{
	int i;
	struct mosquitto__subleaf *leaf;
	struct mosquitto__subhier *hier;

	for(i=0; i<context->sub_count; i++){
		if(context->subs[i] == NULL){
			continue;
		}
		leaf = context->subs[i]->subs;
		while(leaf){
			if(leaf->context==context){
#ifdef WITH_SYS_TREE
				db->subscription_count--;
#endif
				DL_DELETE(context->subs[i]->subs, leaf);
				mosquitto__free(leaf);
				break;
			}
			leaf = leaf->next;
		}
		if(context->subs[i]->subs == NULL
				&& context->subs[i]->children == NULL
				&& context->subs[i]->shared == NULL
				&& context->subs[i]->parent){

			hier = context->subs[i];
			context->subs[i] = NULL;
			do{
				hier = tmp_remove_subs(hier);
			}while(hier);
		}
	}
	mosquitto__free(context->subs);
	context->subs = NULL;
	context->sub_count = 0;

	return sub__clean_session_shared(db, context);
}

void sub__tree_print(struct mosquitto__subhier *root, int level)
{
	int i;
	struct mosquitto__subhier *branch, *branch_tmp;
	struct mosquitto__subleaf *leaf;

	HASH_ITER(hh, root, branch, branch_tmp){
		if(level > -1){
			for(i=0; i<(level+2)*2; i++){
				printf(" ");
			}
			printf("%s", branch->topic);
			leaf = branch->subs;
			while(leaf){
				if(leaf->context){
					printf(" (%s, %d)", leaf->context->id, leaf->qos);
				}else{
					printf(" (%s, %d)", "", leaf->qos);
				}
				leaf = leaf->next;
			}
			printf("\n");
		}

		sub__tree_print(branch->children, level+1);
	}
}

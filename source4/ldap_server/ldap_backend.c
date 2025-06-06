/*
   Unix SMB/CIFS implementation.
   LDAP server
   Copyright (C) Stefan Metzmacher 2004
   Copyright (C) Matthias Dieter Wallnöfer 2009

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; either version 3 of the License, or
   (at your option) any later version.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/

#include "includes.h"
#include <talloc.h>
#include "ldap_server/ldap_server.h"
#include "../lib/util/dlinklist.h"
#include "auth/credentials/credentials.h"
#include "auth/gensec/gensec.h"
#include "auth/common_auth.h"
#include "param/param.h"
#include "samba/service_stream.h"
#include "dsdb/gmsa/util.h"
#include "dsdb/samdb/samdb.h"
#include <ldb_errors.h>
#include <ldb_module.h>
#include "ldb_wrap.h"
#include "lib/tsocket/tsocket.h"
#include "libcli/ldap/ldap_proto.h"
#include "source4/auth/auth.h"

#undef DBGC_CLASS
#define DBGC_CLASS DBGC_LDAPSRV

static int map_ldb_error(TALLOC_CTX *mem_ctx, int ldb_err,
	const char *add_err_string, const char **errstring)
{
	WERROR err;

	/* Certain LDB modules need to return very special WERROR codes. Proof
	 * for them here and if they exist skip the rest of the mapping. */
	if (add_err_string != NULL) {
		char *endptr;
		strtol(add_err_string, &endptr, 16);
		if (endptr != add_err_string) {
			*errstring = add_err_string;
			return ldb_err;
		}
	}

	/* Otherwise we calculate here a generic, but appropriate WERROR. */

	switch (ldb_err) {
	case LDB_SUCCESS:
		err = WERR_OK;
	break;
	case LDB_ERR_OPERATIONS_ERROR:
		err = WERR_DS_OPERATIONS_ERROR;
	break;
	case LDB_ERR_PROTOCOL_ERROR:
		err = WERR_DS_PROTOCOL_ERROR;
	break;
	case LDB_ERR_TIME_LIMIT_EXCEEDED:
		err = WERR_DS_TIMELIMIT_EXCEEDED;
	break;
	case LDB_ERR_SIZE_LIMIT_EXCEEDED:
		err = WERR_DS_SIZELIMIT_EXCEEDED;
	break;
	case LDB_ERR_COMPARE_FALSE:
		err = WERR_DS_COMPARE_FALSE;
	break;
	case LDB_ERR_COMPARE_TRUE:
		err = WERR_DS_COMPARE_TRUE;
	break;
	case LDB_ERR_AUTH_METHOD_NOT_SUPPORTED:
		err = WERR_DS_AUTH_METHOD_NOT_SUPPORTED;
	break;
	case LDB_ERR_STRONG_AUTH_REQUIRED:
		err = WERR_DS_STRONG_AUTH_REQUIRED;
	break;
	case LDB_ERR_REFERRAL:
		err = WERR_DS_REFERRAL;
	break;
	case LDB_ERR_ADMIN_LIMIT_EXCEEDED:
		err = WERR_DS_ADMIN_LIMIT_EXCEEDED;
	break;
	case LDB_ERR_UNSUPPORTED_CRITICAL_EXTENSION:
		err = WERR_DS_UNAVAILABLE_CRIT_EXTENSION;
	break;
	case LDB_ERR_CONFIDENTIALITY_REQUIRED:
		err = WERR_DS_CONFIDENTIALITY_REQUIRED;
	break;
	case LDB_ERR_SASL_BIND_IN_PROGRESS:
		err = WERR_DS_BUSY;
	break;
	case LDB_ERR_NO_SUCH_ATTRIBUTE:
		err = WERR_DS_NO_ATTRIBUTE_OR_VALUE;
	break;
	case LDB_ERR_UNDEFINED_ATTRIBUTE_TYPE:
		err = WERR_DS_ATTRIBUTE_TYPE_UNDEFINED;
	break;
	case LDB_ERR_INAPPROPRIATE_MATCHING:
		err = WERR_DS_INAPPROPRIATE_MATCHING;
	break;
	case LDB_ERR_CONSTRAINT_VIOLATION:
		err = WERR_DS_CONSTRAINT_VIOLATION;
	break;
	case LDB_ERR_ATTRIBUTE_OR_VALUE_EXISTS:
		err = WERR_DS_ATTRIBUTE_OR_VALUE_EXISTS;
	break;
	case LDB_ERR_INVALID_ATTRIBUTE_SYNTAX:
		err = WERR_DS_INVALID_ATTRIBUTE_SYNTAX;
	break;
	case LDB_ERR_NO_SUCH_OBJECT:
		err = WERR_DS_NO_SUCH_OBJECT;
	break;
	case LDB_ERR_ALIAS_PROBLEM:
		err = WERR_DS_ALIAS_PROBLEM;
	break;
	case LDB_ERR_INVALID_DN_SYNTAX:
		err = WERR_DS_INVALID_DN_SYNTAX;
	break;
	case LDB_ERR_ALIAS_DEREFERENCING_PROBLEM:
		err = WERR_DS_ALIAS_DEREF_PROBLEM;
	break;
	case LDB_ERR_INAPPROPRIATE_AUTHENTICATION:
		err = WERR_DS_INAPPROPRIATE_AUTH;
	break;
	case LDB_ERR_INVALID_CREDENTIALS:
		err = WERR_ACCESS_DENIED;
	break;
	case LDB_ERR_INSUFFICIENT_ACCESS_RIGHTS:
		err = WERR_DS_INSUFF_ACCESS_RIGHTS;
	break;
	case LDB_ERR_BUSY:
		err = WERR_DS_BUSY;
	break;
	case LDB_ERR_UNAVAILABLE:
		err = WERR_DS_UNAVAILABLE;
	break;
	case LDB_ERR_UNWILLING_TO_PERFORM:
		err = WERR_DS_UNWILLING_TO_PERFORM;
	break;
	case LDB_ERR_LOOP_DETECT:
		err = WERR_DS_LOOP_DETECT;
	break;
	case LDB_ERR_NAMING_VIOLATION:
		err = WERR_DS_NAMING_VIOLATION;
	break;
	case LDB_ERR_OBJECT_CLASS_VIOLATION:
		err = WERR_DS_OBJ_CLASS_VIOLATION;
	break;
	case LDB_ERR_NOT_ALLOWED_ON_NON_LEAF:
		err = WERR_DS_CANT_ON_NON_LEAF;
	break;
	case LDB_ERR_NOT_ALLOWED_ON_RDN:
		err = WERR_DS_CANT_ON_RDN;
	break;
	case LDB_ERR_ENTRY_ALREADY_EXISTS:
		err = WERR_DS_OBJ_STRING_NAME_EXISTS;
	break;
	case LDB_ERR_OBJECT_CLASS_MODS_PROHIBITED:
		err = WERR_DS_CANT_MOD_OBJ_CLASS;
	break;
	case LDB_ERR_AFFECTS_MULTIPLE_DSAS:
		err = WERR_DS_AFFECTS_MULTIPLE_DSAS;
	break;
	default:
		err = WERR_DS_GENERIC_ERROR;
	break;
	}

	*errstring = talloc_asprintf(mem_ctx, "%08X: %s", W_ERROR_V(err),
		add_err_string != NULL ? add_err_string : ldb_strerror(ldb_err));

	/* result is 1:1 for now */
	return ldb_err;
}

/*
  connect to the sam database
*/
int ldapsrv_backend_Init(struct ldapsrv_connection *conn,
			      char **errstring)
{
	bool using_tls = conn->sockets.active == conn->sockets.tls;
	bool using_seal = conn->gensec != NULL && gensec_have_feature(conn->gensec,
								      GENSEC_FEATURE_SEAL);
	struct dsdb_encrypted_connection_state *opaque_connection_state = NULL;

	int ret = samdb_connect_url(conn,
				    conn->connection->event.ctx,
				    conn->lp_ctx,
				    conn->session_info,
				    conn->global_catalog ? LDB_FLG_RDONLY : 0,
				    "sam.ldb",
				    conn->connection->remote_address,
				    &conn->ldb,
				    errstring);
	if (ret != LDB_SUCCESS) {
		return ret;
	}

	/*
	 * We can safely call ldb_set_opaque() on this ldb as we have
	 * set remote_address above which avoids the ldb handle cache
	 */
	opaque_connection_state = talloc_zero(conn, struct dsdb_encrypted_connection_state);
	if (opaque_connection_state == NULL) {
		return LDB_ERR_OPERATIONS_ERROR;
	}
	opaque_connection_state->using_encrypted_connection = using_tls || using_seal || conn->is_ldapi;
	ret = ldb_set_opaque(conn->ldb,
			     DSDB_OPAQUE_ENCRYPTED_CONNECTION_STATE_NAME,
			     opaque_connection_state);
	if (ret != LDB_SUCCESS) {
		DBG_ERR("ldb_set_opaque() failed to store our "
			"encrypted connection state!\n");
		return ret;
	}

	if (conn->server_credentials) {
		struct gensec_security *gensec_security = NULL;
		const char **sasl_mechs = NULL;
		NTSTATUS status;

		status = samba_server_gensec_start(conn,
						   conn->connection->event.ctx,
						   conn->connection->msg_ctx,
						   conn->lp_ctx,
						   conn->server_credentials,
						   "ldap",
						   &gensec_security);
		if (!NT_STATUS_IS_OK(status)) {
			DBG_ERR("samba_server_gensec_start failed: %s\n",
				nt_errstr(status));
			return LDB_ERR_OPERATIONS_ERROR;
		}

		/* ldb can have a different lifetime to conn, so we
		   need to ensure that sasl_mechs lives as long as the
		   ldb does */
		sasl_mechs = gensec_security_sasl_names(gensec_security,
							conn->ldb);
		TALLOC_FREE(gensec_security);
		if (sasl_mechs == NULL) {
			DBG_ERR("Failed to get sasl mechs!\n");
			return LDB_ERR_OPERATIONS_ERROR;
		}

		ldb_set_opaque(conn->ldb, "supportedSASLMechanisms", sasl_mechs);
	}

	return LDB_SUCCESS;
}

struct ldapsrv_reply *ldapsrv_init_reply(struct ldapsrv_call *call, uint8_t type)
{
	struct ldapsrv_reply *reply;

	reply = talloc_zero(call, struct ldapsrv_reply);
	if (!reply) {
		return NULL;
	}
	reply->msg = talloc_zero(reply, struct ldap_message);
	if (reply->msg == NULL) {
		talloc_free(reply);
		return NULL;
	}

	reply->msg->messageid = call->request->messageid;
	reply->msg->type = type;
	reply->msg->controls = NULL;

	return reply;
}

/*
 * Encode a reply to an LDAP client as ASN.1, free the original memory
 */
static NTSTATUS ldapsrv_encode(TALLOC_CTX *mem_ctx,
			       struct ldapsrv_reply *reply)
{
	bool bret = ldap_encode(reply->msg,
				samba_ldap_control_handlers(),
				&reply->blob,
				mem_ctx);
	if (!bret) {
		DBG_ERR("Failed to encode ldap reply of type %d: "
			 "ldap_encode() failed\n",
			 reply->msg->type);
		TALLOC_FREE(reply->msg);
		return NT_STATUS_NO_MEMORY;
	}

	TALLOC_FREE(reply->msg);
	talloc_set_name_const(reply->blob.data,
			      "Outgoing, encoded single LDAP reply");

	return NT_STATUS_OK;
}

/*
 * Queue a reply (encoding it also), even if it would exceed the
 * limit.  This allows the error packet with LDAP_SIZE_LIMIT_EXCEEDED
 * to be sent
 */
static NTSTATUS ldapsrv_queue_reply_forced(struct ldapsrv_call *call,
					   struct ldapsrv_reply *reply)
{
	NTSTATUS status = ldapsrv_encode(call, reply);

	if (NT_STATUS_IS_OK(status)) {
		DLIST_ADD_END(call->replies, reply);
	}
	return status;
}

/*
 * Queue a reply (encoding it also) but check we do not send more than
 * LDAP_SERVER_MAX_REPLY_SIZE of responses as a way to limit the
 * amount of data a client can make us allocate.
 */
NTSTATUS ldapsrv_queue_reply(struct ldapsrv_call *call, struct ldapsrv_reply *reply)
{
	NTSTATUS status = ldapsrv_encode(call, reply);

	if (!NT_STATUS_IS_OK(status)) {
		return status;
	}

	if (call->reply_size > call->reply_size + reply->blob.length
	    || call->reply_size + reply->blob.length > LDAP_SERVER_MAX_REPLY_SIZE) {
		DBG_WARNING("Refusing to queue LDAP search response size "
			    "of more than %zu bytes\n",
			    LDAP_SERVER_MAX_REPLY_SIZE);
		TALLOC_FREE(reply->blob.data);
		return NT_STATUS_FILE_TOO_LARGE;
	}

	call->reply_size += reply->blob.length;

	DLIST_ADD_END(call->replies, reply);

	return status;
}

static NTSTATUS ldapsrv_unwilling(struct ldapsrv_call *call, int error)
{
	struct ldapsrv_reply *reply;
	struct ldap_ExtendedResponse *r;

	DBG_DEBUG("type[%d] id[%d]\n", call->request->type, call->request->messageid);

	reply = ldapsrv_init_reply(call, LDAP_TAG_ExtendedResponse);
	if (!reply) {
		return NT_STATUS_NO_MEMORY;
	}

	r = &reply->msg->r.ExtendedResponse;
	r->response.resultcode = error;
	r->response.dn = NULL;
	r->response.errormessage = NULL;
	r->response.referral = NULL;
	r->oid = NULL;
	r->value = NULL;

	ldapsrv_queue_reply(call, reply);
	return NT_STATUS_OK;
}

static int ldapsrv_add_with_controls(struct ldapsrv_call *call,
				     const struct ldb_message *message,
				     struct ldb_control **controls,
				     struct ldb_result *res)
{
	struct ldb_context *ldb = call->conn->ldb;
	struct ldb_request *req;
	int ret;

	ret = ldb_msg_sanity_check(ldb, message);
	if (ret != LDB_SUCCESS) {
		return ret;
	}

	ret = ldb_build_add_req(&req, ldb, ldb,
					message,
					controls,
					res,
					ldb_modify_default_callback,
					NULL);

	if (ret != LDB_SUCCESS) return ret;

	if (call->conn->global_catalog) {
		return ldb_error(ldb, LDB_ERR_UNWILLING_TO_PERFORM, "modify forbidden on global catalog port");
	}
	ldb_request_add_control(req, DSDB_CONTROL_NO_GLOBAL_CATALOG, false, NULL);

	ret = ldb_transaction_start(ldb);
	if (ret != LDB_SUCCESS) {
		return ret;
	}

	if (!call->conn->is_privileged) {
		ldb_req_mark_untrusted(req);
	}

	LDB_REQ_SET_LOCATION(req);

	ret = ldb_request(ldb, req);
	if (ret == LDB_SUCCESS) {
		ret = ldb_wait(req->handle, LDB_WAIT_ALL);
	}

	if (ret == LDB_SUCCESS) {
		ret = ldb_transaction_commit(ldb);
	}
	else {
		ldb_transaction_cancel(ldb);
	}

	talloc_free(req);
	return ret;
}

/* create and execute a modify request */
static int ldapsrv_mod_with_controls(struct ldapsrv_call *call,
				     const struct ldb_message *message,
				     struct ldb_control **controls,
				     struct ldb_result *res)
{
	struct ldb_context *ldb = call->conn->ldb;
	struct ldb_request *req;
	int ret;

	ret = ldb_msg_sanity_check(ldb, message);
	if (ret != LDB_SUCCESS) {
		return ret;
	}

	ret = ldb_build_mod_req(&req, ldb, ldb,
					message,
					controls,
					res,
					ldb_modify_default_callback,
					NULL);

	if (ret != LDB_SUCCESS) {
		return ret;
	}

	if (call->conn->global_catalog) {
		return ldb_error(ldb, LDB_ERR_UNWILLING_TO_PERFORM, "modify forbidden on global catalog port");
	}
	ldb_request_add_control(req, DSDB_CONTROL_NO_GLOBAL_CATALOG, false, NULL);

	ret = ldb_transaction_start(ldb);
	if (ret != LDB_SUCCESS) {
		return ret;
	}

	if (!call->conn->is_privileged) {
		ldb_req_mark_untrusted(req);
	}

	LDB_REQ_SET_LOCATION(req);

	ret = ldb_request(ldb, req);
	if (ret == LDB_SUCCESS) {
		ret = ldb_wait(req->handle, LDB_WAIT_ALL);
	}

	if (ret == LDB_SUCCESS) {
		ret = ldb_transaction_commit(ldb);
	}
	else {
		ldb_transaction_cancel(ldb);
	}

	talloc_free(req);
	return ret;
}

/* create and execute a delete request */
static int ldapsrv_del_with_controls(struct ldapsrv_call *call,
				     struct ldb_dn *dn,
				     struct ldb_control **controls,
				     struct ldb_result *res)
{
	struct ldb_context *ldb = call->conn->ldb;
	struct ldb_request *req;
	int ret;

	ret = ldb_build_del_req(&req, ldb, ldb,
					dn,
					controls,
					res,
					ldb_modify_default_callback,
					NULL);

	if (ret != LDB_SUCCESS) return ret;

	if (call->conn->global_catalog) {
		return ldb_error(ldb, LDB_ERR_UNWILLING_TO_PERFORM, "modify forbidden on global catalog port");
	}
	ldb_request_add_control(req, DSDB_CONTROL_NO_GLOBAL_CATALOG, false, NULL);

	ret = ldb_transaction_start(ldb);
	if (ret != LDB_SUCCESS) {
		return ret;
	}

	if (!call->conn->is_privileged) {
		ldb_req_mark_untrusted(req);
	}

	LDB_REQ_SET_LOCATION(req);

	ret = ldb_request(ldb, req);
	if (ret == LDB_SUCCESS) {
		ret = ldb_wait(req->handle, LDB_WAIT_ALL);
	}

	if (ret == LDB_SUCCESS) {
		ret = ldb_transaction_commit(ldb);
	}
	else {
		ldb_transaction_cancel(ldb);
	}

	talloc_free(req);
	return ret;
}

static int ldapsrv_rename_with_controls(struct ldapsrv_call *call,
					struct ldb_dn *olddn,
					struct ldb_dn *newdn,
					struct ldb_control **controls,
					struct ldb_result *res)
{
	struct ldb_context *ldb = call->conn->ldb;
	struct ldb_request *req;
	int ret;

	ret = ldb_build_rename_req(&req, ldb, ldb,
					olddn,
					newdn,
					controls,
					res,
					ldb_modify_default_callback,
					NULL);

	if (ret != LDB_SUCCESS) return ret;

	if (call->conn->global_catalog) {
		return ldb_error(ldb, LDB_ERR_UNWILLING_TO_PERFORM, "modify forbidden on global catalog port");
	}
	ldb_request_add_control(req, DSDB_CONTROL_NO_GLOBAL_CATALOG, false, NULL);

	ret = ldb_transaction_start(ldb);
	if (ret != LDB_SUCCESS) {
		return ret;
	}

	if (!call->conn->is_privileged) {
		ldb_req_mark_untrusted(req);
	}

	LDB_REQ_SET_LOCATION(req);

	ret = ldb_request(ldb, req);
	if (ret == LDB_SUCCESS) {
		ret = ldb_wait(req->handle, LDB_WAIT_ALL);
	}

	if (ret == LDB_SUCCESS) {
		ret = ldb_transaction_commit(ldb);
	}
	else {
		ldb_transaction_cancel(ldb);
	}

	talloc_free(req);
	return ret;
}



struct ldapsrv_context {
	struct ldapsrv_call *call;
	int extended_type;
	bool attributesonly;
	struct ldb_control **controls;
	size_t count; /* For notification only */
	const struct gmsa_update **updates;
};

static int ldap_server_search_callback(struct ldb_request *req, struct ldb_reply *ares)
{
	struct ldapsrv_context *ctx = talloc_get_type(req->context, struct ldapsrv_context);
	struct ldapsrv_call *call = ctx->call;
	struct ldb_context *ldb = call->conn->ldb;
	unsigned int j;
	struct ldapsrv_reply *ent_r = NULL;
	struct ldap_SearchResEntry *ent;
	int ret;
	NTSTATUS status;

	if (!ares) {
		return ldb_request_done(req, LDB_ERR_OPERATIONS_ERROR);
	}
	if (ares->error != LDB_SUCCESS) {
		return ldb_request_done(req, ares->error);
	}

	switch (ares->type) {
	case LDB_REPLY_ENTRY:
	{
		struct ldb_message *msg = ares->message;
		ent_r = ldapsrv_init_reply(call, LDAP_TAG_SearchResultEntry);
		if (ent_r == NULL) {
			return ldb_oom(ldb);
		}

		ctx->count++;

		/*
		 * Put the LDAP search response data under ent_r->msg
		 * so we can free that later once encoded
		 */
		talloc_steal(ent_r->msg, msg);

		ent = &ent_r->msg->r.SearchResultEntry;
		ent->dn = ldb_dn_get_extended_linearized(ent_r, msg->dn,
							 ctx->extended_type);
		ent->num_attributes = 0;
		ent->attributes = NULL;
		if (msg->num_elements == 0) {
			goto queue_reply;
		}
		ent->num_attributes = msg->num_elements;
		ent->attributes = talloc_array(ent_r, struct ldb_message_element, ent->num_attributes);
		if (ent->attributes == NULL) {
			return ldb_oom(ldb);
		}

		for (j=0; j < ent->num_attributes; j++) {
			ent->attributes[j].name = msg->elements[j].name;
			ent->attributes[j].num_values = 0;
			ent->attributes[j].values = NULL;
			if (ctx->attributesonly && (msg->elements[j].num_values == 0)) {
				continue;
			}
			ent->attributes[j].num_values = msg->elements[j].num_values;
			ent->attributes[j].values = msg->elements[j].values;
		}

		{
			const struct ldb_control
				*ctrl = ldb_controls_get_control(
					ares->controls,
					DSDB_CONTROL_GMSA_UPDATE_OID);

			if (ctrl != NULL) {
				const struct gmsa_update **updates = NULL;
				const size_t len = talloc_array_length(
					ctx->updates);

				updates = talloc_realloc(
					ctx,
					ctx->updates,
					const struct gmsa_update *,
					len + 1);
				if (updates != NULL) {
					updates[len] = talloc_steal(updates,
								    ctrl->data);
					ctx->updates = updates;
				}
			}
		}

queue_reply:
		status = ldapsrv_queue_reply(call, ent_r);
		if (NT_STATUS_EQUAL(status, NT_STATUS_FILE_TOO_LARGE)) {
			ret = ldb_request_done(req,
					       LDB_ERR_SIZE_LIMIT_EXCEEDED);
			ldb_asprintf_errstring(ldb,
					       "LDAP search response size "
					       "limited to %zu bytes\n",
					       LDAP_SERVER_MAX_REPLY_SIZE);
		} else if (!NT_STATUS_IS_OK(status)) {
			ret = ldb_request_done(req,
					       ldb_operr(ldb));
		} else {
			ret = LDB_SUCCESS;
		}
		break;
	}
	case LDB_REPLY_REFERRAL:
	{
		struct ldap_SearchResRef *ent_ref;

		/*
		 * TODO: This should be handled by the notification
		 * module not here
		 */
		if (call->notification.busy) {
			ret = LDB_SUCCESS;
			break;
		}

		ent_r = ldapsrv_init_reply(call, LDAP_TAG_SearchResultReference);
		if (ent_r == NULL) {
			return ldb_oom(ldb);
		}

		/*
		 * Put the LDAP referral data under ent_r->msg
		 * so we can free that later once encoded
		 */
		talloc_steal(ent_r->msg, ares->referral);

		ent_ref = &ent_r->msg->r.SearchResultReference;
		ent_ref->referral = ares->referral;

		status = ldapsrv_queue_reply(call, ent_r);
		if (!NT_STATUS_IS_OK(status)) {
			ret = LDB_ERR_OPERATIONS_ERROR;
		} else {
			ret = LDB_SUCCESS;
		}
		break;
	}
	case LDB_REPLY_DONE:
	{
		/*
		 * We don't queue the reply for this one, we let that
		 * happen outside
		 */
		ctx->controls = talloc_move(ctx, &ares->controls);

		TALLOC_FREE(ares);
		return ldb_request_done(req, LDB_SUCCESS);
	}
	default:
		/* Doesn't happen */
		ret = LDB_ERR_OPERATIONS_ERROR;
	}
	TALLOC_FREE(ares);

	return ret;
}


static NTSTATUS ldapsrv_SearchRequest(struct ldapsrv_call *call)
{
	struct ldap_SearchRequest *req = &call->request->r.SearchRequest;
	struct ldap_Result *done;
	struct ldapsrv_reply *done_r;
	TALLOC_CTX *local_ctx;
	struct ldapsrv_context *callback_ctx = NULL;
	struct ldb_context *samdb = talloc_get_type(call->conn->ldb, struct ldb_context);
	struct ldb_dn *basedn;
	struct ldb_request *lreq;
	struct ldb_control *search_control;
	struct ldb_search_options_control *search_options;
	struct ldb_control *extended_dn_control;
	struct ldb_extended_dn_control *extended_dn_decoded = NULL;
	struct ldb_control *notification_control = NULL;
	enum ldb_scope scope = LDB_SCOPE_DEFAULT;
	const char **attrs = NULL;
	const char *scope_str, *errstr = NULL;
	int result = -1;
	int ldb_ret = -1;
	unsigned int i;
	int extended_type = 1;

	/*
	 * Warn for searches that are longer than 1/4 of the
	 * search_timeout, being 30sec by default
	 */
	struct timeval start_time = timeval_current();
	struct timeval warning_time
		= timeval_add(&start_time,
			      call->conn->limits.search_timeout / 4,
			      0);

	local_ctx = talloc_new(call);
	NT_STATUS_HAVE_NO_MEMORY(local_ctx);

	basedn = ldb_dn_new(local_ctx, samdb, req->basedn);
	NT_STATUS_HAVE_NO_MEMORY(basedn);

	switch (req->scope) {
	case LDAP_SEARCH_SCOPE_BASE:
		scope = LDB_SCOPE_BASE;
		break;
	case LDAP_SEARCH_SCOPE_SINGLE:
		scope = LDB_SCOPE_ONELEVEL;
		break;
	case LDAP_SEARCH_SCOPE_SUB:
		scope = LDB_SCOPE_SUBTREE;
		break;
	default:
		result = LDAP_PROTOCOL_ERROR;
		map_ldb_error(local_ctx, LDB_ERR_PROTOCOL_ERROR, NULL,
			      &errstr);
		scope_str = "<Invalid scope>";
		errstr = talloc_asprintf(local_ctx,
					 "%s. Invalid scope", errstr);
		goto reply;
	}
	scope_str = dsdb_search_scope_as_string(scope);

	DBG_DEBUG("scope: [%s]\n", scope_str);

	if (req->num_attributes >= 1) {
		attrs = talloc_array(local_ctx, const char *, req->num_attributes+1);
		NT_STATUS_HAVE_NO_MEMORY(attrs);

		for (i=0; i < req->num_attributes; i++) {
			DBG_DEBUG("attrs: [%s]\n",req->attributes[i]);
			attrs[i] = req->attributes[i];
		}
		attrs[i] = NULL;
	}

	DBG_INFO("ldb_request %s dn=%s filter=%s\n",
		 scope_str, req->basedn, ldb_filter_from_tree(call, req->tree));

	callback_ctx = talloc_zero(local_ctx, struct ldapsrv_context);
	NT_STATUS_HAVE_NO_MEMORY(callback_ctx);
	callback_ctx->call = call;
	callback_ctx->extended_type = extended_type;
	callback_ctx->attributesonly = req->attributesonly;

	ldb_ret = ldb_build_search_req_ex(&lreq, samdb, local_ctx,
					  basedn, scope,
					  req->tree, attrs,
					  call->request->controls,
					  callback_ctx,
					  ldap_server_search_callback,
					  NULL);

	if (ldb_ret != LDB_SUCCESS) {
		goto reply;
	}

	if (call->conn->global_catalog) {
		search_control = ldb_request_get_control(lreq, LDB_CONTROL_SEARCH_OPTIONS_OID);

		search_options = NULL;
		if (search_control != NULL && search_control->data != NULL) {
			search_options = talloc_get_type(search_control->data, struct ldb_search_options_control);
			search_options->search_options |= LDB_SEARCH_OPTION_PHANTOM_ROOT;
		} else {
			search_options = talloc(lreq, struct ldb_search_options_control);
			NT_STATUS_HAVE_NO_MEMORY(search_options);
			search_options->search_options = LDB_SEARCH_OPTION_PHANTOM_ROOT;
			ldb_request_replace_control(
				lreq,
				LDB_CONTROL_SEARCH_OPTIONS_OID,
				false,
				search_options);
		}
	} else {
		ldb_request_add_control(lreq, DSDB_CONTROL_NO_GLOBAL_CATALOG, false, NULL);
	}

	extended_dn_control = ldb_request_get_control(lreq, LDB_CONTROL_EXTENDED_DN_OID);

	if (extended_dn_control) {
		if (extended_dn_control->data) {
			extended_dn_decoded = talloc_get_type(extended_dn_control->data, struct ldb_extended_dn_control);
			extended_type = extended_dn_decoded->type;
		} else {
			extended_type = 0;
		}
		callback_ctx->extended_type = extended_type;
	}

	notification_control = ldb_request_get_control(lreq, LDB_CONTROL_NOTIFICATION_OID);
	if (notification_control != NULL) {
		const struct ldapsrv_call *pc = NULL;
		size_t count = 0;

		for (pc = call->conn->pending_calls; pc != NULL; pc = pc->next) {
			count += 1;
		}

		if (count >= call->conn->limits.max_notifications) {
			DBG_DEBUG("error MaxNotificationPerConn\n");
			result = map_ldb_error(local_ctx,
					       LDB_ERR_ADMIN_LIMIT_EXCEEDED,
					       "MaxNotificationPerConn reached",
					       &errstr);
			goto reply;
		}

		/*
		 * For now we need to do periodic retries on our own.
		 * As the dsdb_notification module will return after each run.
		 */
		call->notification.busy = true;
	}

	{
		const char *scheme = NULL;
		switch (call->conn->referral_scheme) {
		case LDAP_REFERRAL_SCHEME_LDAPS:
			scheme = "ldaps";
			break;
		default:
			scheme = "ldap";
		}
		ldb_ret = ldb_set_opaque(
			samdb,
			LDAP_REFERRAL_SCHEME_OPAQUE,
			discard_const_p(char *, scheme));
		if (ldb_ret != LDB_SUCCESS) {
			goto reply;
		}
	}

	{
		time_t timeout = call->conn->limits.search_timeout;

		if (timeout == 0
		    || (req->timelimit != 0
			&& req->timelimit < timeout))
		{
			timeout = req->timelimit;
		}
		ldb_set_timeout(samdb, lreq, timeout);
	}

	if (!call->conn->is_privileged) {
		ldb_req_mark_untrusted(lreq);
	}

	LDB_REQ_SET_LOCATION(lreq);

	ldb_ret = ldb_request(samdb, lreq);

	if (ldb_ret != LDB_SUCCESS) {
		goto reply;
	}

	ldb_ret = ldb_wait(lreq->handle, LDB_WAIT_ALL);

	if (ldb_ret == LDB_SUCCESS) {
		size_t n;
		const size_t len = talloc_array_length(callback_ctx->updates);

		for (n = 0; n < len; ++n) {
			int ret;

			ret = dsdb_update_gmsa_entry_keys(local_ctx,
							  samdb,
							  callback_ctx->updates[n]);
			if (ret) {
				/* Ignore the error. */
				DBG_WARNING("Failed to update keys for Group "
					    "Managed Service Account: %s\n",
					    ldb_strerror(ret));
			}
		}

		if (call->notification.busy) {
			/* Move/Add it to the end */
			DLIST_DEMOTE(call->conn->pending_calls, call);
			call->notification.generation =
				call->conn->service->notification.generation;

			if (callback_ctx->count != 0) {
				call->notification.generation += 1;
				ldapsrv_notification_retry_setup(call->conn->service,
								 true);
			}

			talloc_free(local_ctx);
			return NT_STATUS_OK;
		}
	}

reply:

	/*
	 * This looks like duplicated code - because it is - but
	 * otherwise the work in the parameters will be done
	 * regardless, this way the functions only execute when the
	 * log level is set.
	 *
	 * The basedn is re-obtained as a string to escape it
	 */
	if ((req->timelimit == 0 || call->conn->limits.search_timeout < req->timelimit)
	    && ldb_ret == LDB_ERR_TIME_LIMIT_EXCEEDED) {
		struct dom_sid_buf sid_buf;
		DBG_WARNING("MaxQueryDuration(%d) timeout exceeded "
			    "in SearchRequest by %s from %s filter: [%s] "
			    "basedn: [%s] "
			    "scope: [%s]\n",
			    call->conn->limits.search_timeout,
			    dom_sid_str_buf(&call->conn->session_info->security_token->sids[0],
					    &sid_buf),
			    tsocket_address_string(call->conn->connection->remote_address,
						   call),
			    ldb_filter_from_tree(call, req->tree),
			    ldb_dn_get_extended_linearized(call, basedn, 1),
			    scope_str);
		for (i=0; i < req->num_attributes; i++) {
			DBG_WARNING("MaxQueryDuration timeout exceeded attrs: [%s]\n",
				    req->attributes[i]);
		}

	} else if (timeval_expired(&warning_time)) {
		struct dom_sid_buf sid_buf;
		DBG_NOTICE("Long LDAP Query: Duration was %.2fs, "
			   "MaxQueryDuration(%d)/4 == %d "
			   "in SearchRequest by %s from %s filter: [%s] "
			   "basedn: [%s] "
			   "scope: [%s] "
			   "result: %s\n",
			   timeval_elapsed(&start_time),
			   call->conn->limits.search_timeout,
			   call->conn->limits.search_timeout / 4,
			   dom_sid_str_buf(&call->conn->session_info->security_token->sids[0],
					   &sid_buf),
			   tsocket_address_string(call->conn->connection->remote_address,
						  call),
			   ldb_filter_from_tree(call, req->tree),
			   ldb_dn_get_extended_linearized(call, basedn, 1),
			   scope_str,
			   ldb_strerror(ldb_ret));
		for (i=0; i < req->num_attributes; i++) {
			DBG_NOTICE("Long LDAP Query attrs: [%s]\n",
				   req->attributes[i]);
		}
	} else {
		struct dom_sid_buf sid_buf;
		DBG_INFO("LDAP Query: Duration was %.2fs, "
			 "SearchRequest by %s from %s filter: [%s] "
			 "basedn: [%s] "
			 "scope: [%s] "
			 "result: %s\n",
			 timeval_elapsed(&start_time),
			 dom_sid_str_buf(&call->conn->session_info->security_token->sids[0],
					 &sid_buf),
			 tsocket_address_string(call->conn->connection->remote_address,
						call),
			 ldb_filter_from_tree(call, req->tree),
			 ldb_dn_get_extended_linearized(call, basedn, 1),
			 scope_str,
			 ldb_strerror(ldb_ret));
	}

	DLIST_REMOVE(call->conn->pending_calls, call);
	call->notification.busy = false;

	done_r = ldapsrv_init_reply(call, LDAP_TAG_SearchResultDone);
	NT_STATUS_HAVE_NO_MEMORY(done_r);

	done = &done_r->msg->r.SearchResultDone;
	done->dn = NULL;
	done->referral = NULL;

	if (result != -1) {
	} else if (ldb_ret == LDB_SUCCESS) {
		if (callback_ctx->controls) {
			done_r->msg->controls = callback_ctx->controls;
			talloc_steal(done_r->msg, callback_ctx->controls);
		}
		result = LDB_SUCCESS;
	} else {
		DBG_DEBUG("error\n");
		result = map_ldb_error(local_ctx, ldb_ret, ldb_errstring(samdb),
				       &errstr);
	}

	done->resultcode = result;
	done->errormessage = (errstr?talloc_strdup(done_r, errstr):NULL);

	talloc_free(local_ctx);

	return ldapsrv_queue_reply_forced(call, done_r);
}

static NTSTATUS ldapsrv_ModifyRequest(struct ldapsrv_call *call)
{
	struct ldap_ModifyRequest *req = &call->request->r.ModifyRequest;
	struct ldap_Result *modify_result;
	struct ldapsrv_reply *modify_reply;
	TALLOC_CTX *local_ctx;
	struct ldb_context *samdb = call->conn->ldb;
	struct ldb_message *msg = NULL;
	struct ldb_dn *dn;
	const char *errstr = NULL;
	int result = LDAP_SUCCESS;
	int ldb_ret;
	unsigned int i,j;
	struct ldb_result *res = NULL;

	DBG_DEBUG("dn: %s\n", req->dn);

	local_ctx = talloc_named(call, 0, "ModifyRequest local memory context");
	NT_STATUS_HAVE_NO_MEMORY(local_ctx);

	dn = ldb_dn_new(local_ctx, samdb, req->dn);
	NT_STATUS_HAVE_NO_MEMORY(dn);

	DBG_DEBUG("dn: [%s]\n", req->dn);

	msg = ldb_msg_new(local_ctx);
	NT_STATUS_HAVE_NO_MEMORY(msg);

	msg->dn = dn;

	if (req->num_mods > 0) {
		msg->num_elements = req->num_mods;
		msg->elements = talloc_array(msg, struct ldb_message_element, req->num_mods);
		NT_STATUS_HAVE_NO_MEMORY(msg->elements);

		for (i=0; i < msg->num_elements; i++) {
			msg->elements[i].name = discard_const_p(char, req->mods[i].attrib.name);
			msg->elements[i].num_values = 0;
			msg->elements[i].values = NULL;

			switch (req->mods[i].type) {
			default:
				result = LDAP_PROTOCOL_ERROR;
				map_ldb_error(local_ctx,
					LDB_ERR_PROTOCOL_ERROR, NULL, &errstr);
				errstr = talloc_asprintf(local_ctx,
					"%s. Invalid LDAP_MODIFY_* type", errstr);
				goto reply;
			case LDAP_MODIFY_ADD:
				msg->elements[i].flags = LDB_FLAG_MOD_ADD;
				break;
			case LDAP_MODIFY_DELETE:
				msg->elements[i].flags = LDB_FLAG_MOD_DELETE;
				break;
			case LDAP_MODIFY_REPLACE:
				msg->elements[i].flags = LDB_FLAG_MOD_REPLACE;
				break;
			}

			msg->elements[i].num_values = req->mods[i].attrib.num_values;
			if (msg->elements[i].num_values > 0) {
				msg->elements[i].values = talloc_array(msg->elements, struct ldb_val,
								       msg->elements[i].num_values);
				NT_STATUS_HAVE_NO_MEMORY(msg->elements[i].values);

				for (j=0; j < msg->elements[i].num_values; j++) {
					msg->elements[i].values[j].length = req->mods[i].attrib.values[j].length;
					msg->elements[i].values[j].data = req->mods[i].attrib.values[j].data;
				}
			}
		}
	}

reply:
	modify_reply = ldapsrv_init_reply(call, LDAP_TAG_ModifyResponse);
	NT_STATUS_HAVE_NO_MEMORY(modify_reply);

	if (result == LDAP_SUCCESS) {
		res = talloc_zero(local_ctx, struct ldb_result);
		NT_STATUS_HAVE_NO_MEMORY(res);
		ldb_ret = ldapsrv_mod_with_controls(call, msg, call->request->controls, res);
		result = map_ldb_error(local_ctx, ldb_ret, ldb_errstring(samdb),
				       &errstr);
	}

	modify_result = &modify_reply->msg->r.ModifyResponse;
	modify_result->dn = NULL;
	if ((res != NULL) && (res->refs != NULL)) {
		modify_result->resultcode = map_ldb_error(local_ctx,
							  LDB_ERR_REFERRAL,
							  NULL, &errstr);
		modify_result->errormessage = (errstr?talloc_strdup(modify_reply, errstr):NULL);
		modify_result->referral = talloc_strdup(call, *res->refs);
	} else {
		modify_result->resultcode = result;
		modify_result->errormessage = (errstr?talloc_strdup(modify_reply, errstr):NULL);
		modify_result->referral = NULL;
	}
	talloc_free(local_ctx);

	return ldapsrv_queue_reply(call, modify_reply);

}

static NTSTATUS ldapsrv_AddRequest(struct ldapsrv_call *call)
{
	struct ldap_AddRequest *req = &call->request->r.AddRequest;
	struct ldap_Result *add_result;
	struct ldapsrv_reply *add_reply;
	TALLOC_CTX *local_ctx;
	struct ldb_context *samdb = call->conn->ldb;
	struct ldb_message *msg = NULL;
	struct ldb_dn *dn;
	const char *errstr = NULL;
	int result = LDAP_SUCCESS;
	int ldb_ret;
	unsigned int i,j;
	struct ldb_result *res = NULL;

	DBG_DEBUG("dn: %s\n", req->dn);

	local_ctx = talloc_named(call, 0, "AddRequest local memory context");
	NT_STATUS_HAVE_NO_MEMORY(local_ctx);

	dn = ldb_dn_new(local_ctx, samdb, req->dn);
	NT_STATUS_HAVE_NO_MEMORY(dn);

	DBG_DEBUG("dn: [%s]\n", req->dn);

	msg = talloc(local_ctx, struct ldb_message);
	NT_STATUS_HAVE_NO_MEMORY(msg);

	msg->dn = dn;
	msg->num_elements = 0;
	msg->elements = NULL;

	if (req->num_attributes > 0) {
		msg->num_elements = req->num_attributes;
		msg->elements = talloc_array(msg, struct ldb_message_element, msg->num_elements);
		NT_STATUS_HAVE_NO_MEMORY(msg->elements);

		for (i=0; i < msg->num_elements; i++) {
			msg->elements[i].name = discard_const_p(char, req->attributes[i].name);
			msg->elements[i].flags = 0;
			msg->elements[i].num_values = 0;
			msg->elements[i].values = NULL;

			if (req->attributes[i].num_values > 0) {
				msg->elements[i].num_values = req->attributes[i].num_values;
				msg->elements[i].values = talloc_array(msg->elements, struct ldb_val,
								       msg->elements[i].num_values);
				NT_STATUS_HAVE_NO_MEMORY(msg->elements[i].values);

				for (j=0; j < msg->elements[i].num_values; j++) {
					msg->elements[i].values[j].length = req->attributes[i].values[j].length;
					msg->elements[i].values[j].data = req->attributes[i].values[j].data;
				}
			}
		}
	}

	add_reply = ldapsrv_init_reply(call, LDAP_TAG_AddResponse);
	NT_STATUS_HAVE_NO_MEMORY(add_reply);

	if (result == LDAP_SUCCESS) {
		res = talloc_zero(local_ctx, struct ldb_result);
		NT_STATUS_HAVE_NO_MEMORY(res);
		ldb_ret = ldapsrv_add_with_controls(call, msg, call->request->controls, res);
		result = map_ldb_error(local_ctx, ldb_ret, ldb_errstring(samdb),
				       &errstr);
	}

	add_result = &add_reply->msg->r.AddResponse;
	add_result->dn = NULL;
	if ((res != NULL) && (res->refs != NULL)) {
		add_result->resultcode =  map_ldb_error(local_ctx,
							LDB_ERR_REFERRAL, NULL,
							&errstr);
		add_result->errormessage = (errstr?talloc_strdup(add_reply,errstr):NULL);
		add_result->referral = talloc_strdup(call, *res->refs);
	} else {
		add_result->resultcode = result;
		add_result->errormessage = (errstr?talloc_strdup(add_reply,errstr):NULL);
		add_result->referral = NULL;
	}
	talloc_free(local_ctx);

	return ldapsrv_queue_reply(call, add_reply);

}

static NTSTATUS ldapsrv_DelRequest(struct ldapsrv_call *call)
{
	struct ldap_DelRequest *req = &call->request->r.DelRequest;
	struct ldap_Result *del_result;
	struct ldapsrv_reply *del_reply;
	TALLOC_CTX *local_ctx;
	struct ldb_context *samdb = call->conn->ldb;
	struct ldb_dn *dn;
	const char *errstr = NULL;
	int result = LDAP_SUCCESS;
	int ldb_ret;
	struct ldb_result *res = NULL;

	DBG_DEBUG("dn: %s\n", req->dn);

	local_ctx = talloc_named(call, 0, "DelRequest local memory context");
	NT_STATUS_HAVE_NO_MEMORY(local_ctx);

	dn = ldb_dn_new(local_ctx, samdb, req->dn);
	NT_STATUS_HAVE_NO_MEMORY(dn);

	DBG_DEBUG("dn: [%s]\n", req->dn);

	del_reply = ldapsrv_init_reply(call, LDAP_TAG_DelResponse);
	NT_STATUS_HAVE_NO_MEMORY(del_reply);

	if (result == LDAP_SUCCESS) {
		res = talloc_zero(local_ctx, struct ldb_result);
		NT_STATUS_HAVE_NO_MEMORY(res);
		ldb_ret = ldapsrv_del_with_controls(call, dn, call->request->controls, res);
		result = map_ldb_error(local_ctx, ldb_ret, ldb_errstring(samdb),
				       &errstr);
	}

	del_result = &del_reply->msg->r.DelResponse;
	del_result->dn = NULL;
	if ((res != NULL) && (res->refs != NULL)) {
		del_result->resultcode = map_ldb_error(local_ctx,
						       LDB_ERR_REFERRAL, NULL,
						       &errstr);
		del_result->errormessage = (errstr?talloc_strdup(del_reply,errstr):NULL);
		del_result->referral = talloc_strdup(call, *res->refs);
	} else {
		del_result->resultcode = result;
		del_result->errormessage = (errstr?talloc_strdup(del_reply,errstr):NULL);
		del_result->referral = NULL;
	}

	talloc_free(local_ctx);

	return ldapsrv_queue_reply(call, del_reply);
}

static NTSTATUS ldapsrv_ModifyDNRequest(struct ldapsrv_call *call)
{
	struct ldap_ModifyDNRequest *req = &call->request->r.ModifyDNRequest;
	struct ldap_Result *modifydn;
	struct ldapsrv_reply *modifydn_r;
	TALLOC_CTX *local_ctx;
	struct ldb_context *samdb = call->conn->ldb;
	struct ldb_dn *olddn, *newdn=NULL, *newrdn;
	struct ldb_dn *parentdn = NULL;
	const char *errstr = NULL;
	int result = LDAP_SUCCESS;
	int ldb_ret;
	struct ldb_result *res = NULL;

	DBG_DEBUG("dn: %s newrdn: %s\n",
		  req->dn, req->newrdn);

	local_ctx = talloc_named(call, 0, "ModifyDNRequest local memory context");
	NT_STATUS_HAVE_NO_MEMORY(local_ctx);

	olddn = ldb_dn_new(local_ctx, samdb, req->dn);
	NT_STATUS_HAVE_NO_MEMORY(olddn);

	newrdn = ldb_dn_new(local_ctx, samdb, req->newrdn);
	NT_STATUS_HAVE_NO_MEMORY(newrdn);

	DBG_DEBUG("olddn: [%s] newrdn: [%s]\n",
		  req->dn, req->newrdn);

	if (ldb_dn_get_comp_num(newrdn) == 0) {
		result = LDAP_PROTOCOL_ERROR;
		map_ldb_error(local_ctx, LDB_ERR_PROTOCOL_ERROR, NULL,
			      &errstr);
		goto reply;
	}

	if (ldb_dn_get_comp_num(newrdn) > 1) {
		result = LDAP_NAMING_VIOLATION;
		map_ldb_error(local_ctx, LDB_ERR_NAMING_VIOLATION, NULL,
			      &errstr);
		goto reply;
	}

	/* we can't handle the rename if we should not remove the old dn */
	if (!req->deleteolddn) {
		result = LDAP_UNWILLING_TO_PERFORM;
		map_ldb_error(local_ctx, LDB_ERR_UNWILLING_TO_PERFORM, NULL,
			      &errstr);
		errstr = talloc_asprintf(local_ctx,
			"%s. Old RDN must be deleted", errstr);
		goto reply;
	}

	if (req->newsuperior) {
		DBG_DEBUG("newsuperior: [%s]\n", req->newsuperior);
		parentdn = ldb_dn_new(local_ctx, samdb, req->newsuperior);
	}

	if (!parentdn) {
		parentdn = ldb_dn_get_parent(local_ctx, olddn);
	}
	if (!parentdn) {
		result = LDAP_NO_SUCH_OBJECT;
		map_ldb_error(local_ctx, LDB_ERR_NO_SUCH_OBJECT, NULL, &errstr);
		goto reply;
	}

	if ( ! ldb_dn_add_child(parentdn, newrdn)) {
		result = LDAP_OTHER;
		map_ldb_error(local_ctx, LDB_ERR_OTHER, NULL, &errstr);
		goto reply;
	}
	newdn = parentdn;

reply:
	modifydn_r = ldapsrv_init_reply(call, LDAP_TAG_ModifyDNResponse);
	NT_STATUS_HAVE_NO_MEMORY(modifydn_r);

	if (result == LDAP_SUCCESS) {
		res = talloc_zero(local_ctx, struct ldb_result);
		NT_STATUS_HAVE_NO_MEMORY(res);
		ldb_ret = ldapsrv_rename_with_controls(call, olddn, newdn, call->request->controls, res);
		result = map_ldb_error(local_ctx, ldb_ret, ldb_errstring(samdb),
				       &errstr);
	}

	modifydn = &modifydn_r->msg->r.ModifyDNResponse;
	modifydn->dn = NULL;
	if ((res != NULL) && (res->refs != NULL)) {
		modifydn->resultcode = map_ldb_error(local_ctx,
						     LDB_ERR_REFERRAL, NULL,
						     &errstr);;
		modifydn->errormessage = (errstr?talloc_strdup(modifydn_r,errstr):NULL);
		modifydn->referral = talloc_strdup(call, *res->refs);
	} else {
		modifydn->resultcode = result;
		modifydn->errormessage = (errstr?talloc_strdup(modifydn_r,errstr):NULL);
		modifydn->referral = NULL;
	}

	talloc_free(local_ctx);

	return ldapsrv_queue_reply(call, modifydn_r);
}

static NTSTATUS ldapsrv_CompareRequest(struct ldapsrv_call *call)
{
	struct ldap_CompareRequest *req = &call->request->r.CompareRequest;
	struct ldap_Result *compare;
	struct ldapsrv_reply *compare_r;
	TALLOC_CTX *local_ctx;
	struct ldb_context *samdb = call->conn->ldb;
	struct ldb_result *res = NULL;
	struct ldb_dn *dn;
	const char *attrs[1];
	const char *errstr = NULL;
	const char *filter = NULL;
	int result = LDAP_SUCCESS;
	int ldb_ret;

	DBG_DEBUG("dn: %s\n", req->dn);

	local_ctx = talloc_named(call, 0, "CompareRequest local_memory_context");
	NT_STATUS_HAVE_NO_MEMORY(local_ctx);

	dn = ldb_dn_new(local_ctx, samdb, req->dn);
	NT_STATUS_HAVE_NO_MEMORY(dn);

	DBG_DEBUG("dn: [%s]\n", req->dn);
	filter = talloc_asprintf(local_ctx, "(%s=%*s)", req->attribute,
				 (int)req->value.length, req->value.data);
	NT_STATUS_HAVE_NO_MEMORY(filter);

	DBG_DEBUG("attribute: [%s]\n", filter);

	attrs[0] = NULL;

	compare_r = ldapsrv_init_reply(call, LDAP_TAG_CompareResponse);
	NT_STATUS_HAVE_NO_MEMORY(compare_r);

	if (result == LDAP_SUCCESS) {
		ldb_ret = ldb_search(samdb, local_ctx, &res,
				     dn, LDB_SCOPE_BASE, attrs, "%s", filter);
		if (ldb_ret != LDB_SUCCESS) {
			result = map_ldb_error(local_ctx, ldb_ret,
					       ldb_errstring(samdb), &errstr);
			DBG_DEBUG("error: %s\n", errstr);
		} else if (res->count == 0) {
			DBG_DEBUG("didn't match\n");
			result = LDAP_COMPARE_FALSE;
			errstr = NULL;
		} else if (res->count == 1) {
			DBG_DEBUG("matched\n");
			result = LDAP_COMPARE_TRUE;
			errstr = NULL;
		} else if (res->count > 1) {
			result = LDAP_OTHER;
			map_ldb_error(local_ctx, LDB_ERR_OTHER, NULL, &errstr);
			errstr = talloc_asprintf(local_ctx,
				"%s. Too many objects match!", errstr);
			DBG_DEBUG("%u results: %s\n", res->count, errstr);
		}
	}

	compare = &compare_r->msg->r.CompareResponse;
	compare->dn = NULL;
	compare->resultcode = result;
	compare->errormessage = (errstr?talloc_strdup(compare_r,errstr):NULL);
	compare->referral = NULL;

	talloc_free(local_ctx);

	return ldapsrv_queue_reply(call, compare_r);
}

static NTSTATUS ldapsrv_AbandonRequest(struct ldapsrv_call *call)
{
	struct ldap_AbandonRequest *req = &call->request->r.AbandonRequest;
	struct ldapsrv_call *c = NULL;
	struct ldapsrv_call *n = NULL;

	DBG_DEBUG("abandoned\n");

	for (c = call->conn->pending_calls; c != NULL; c = n) {
		n = c->next;

		if (c->request->messageid != req->messageid) {
			continue;
		}

		DLIST_REMOVE(call->conn->pending_calls, c);
		TALLOC_FREE(c);
	}

	return NT_STATUS_OK;
}

static NTSTATUS ldapsrv_expired(struct ldapsrv_call *call)
{
	struct ldapsrv_reply *reply = NULL;
	struct ldap_ExtendedResponse *r = NULL;

	DBG_DEBUG("Sending connection expired message\n");

	reply = ldapsrv_init_reply(call, LDAP_TAG_ExtendedResponse);
	if (reply == NULL) {
		return NT_STATUS_NO_MEMORY;
	}

	/*
	 * According to RFC4511 section 4.4.1 this has a msgid of 0
	 */
	reply->msg->messageid = 0;

	r = &reply->msg->r.ExtendedResponse;
	r->response.resultcode = LDB_ERR_UNAVAILABLE;
	r->response.errormessage = "The server has timed out this connection";
	r->oid = "1.3.6.1.4.1.1466.20036"; /* see rfc4511 section 4.4.1 */

	ldapsrv_queue_reply(call, reply);
	return NT_STATUS_OK;
}

NTSTATUS ldapsrv_do_call(struct ldapsrv_call *call)
{
	unsigned int i;
	struct ldap_message *msg = call->request;
	struct ldapsrv_connection *conn = call->conn;
	NTSTATUS status;
	bool expired;

	expired = timeval_expired(&conn->limits.expire_time);
	if (expired) {
		status = ldapsrv_expired(call);
		if (!NT_STATUS_IS_OK(status)) {
			return status;
		}
		return NT_STATUS_NETWORK_SESSION_EXPIRED;
	}

	/* Check for undecoded critical extensions */
	for (i=0; msg->controls && msg->controls[i]; i++) {
		if (!msg->controls_decoded[i] &&
		    msg->controls[i]->critical) {
			DBG_NOTICE("Critical extension %s is not known to this server\n",
				  msg->controls[i]->oid);
			return ldapsrv_unwilling(call, LDAP_UNAVAILABLE_CRITICAL_EXTENSION);
		}
	}

	if (call->conn->authz_logged == false) {
		bool log = true;

		/*
		 * We do not want to log anonymous access if the query
		 * is just for the rootDSE, or it is a startTLS or a
		 * Bind.
		 *
		 * A rootDSE search could also be done over
		 * CLDAP anonymously for example, so these don't
		 * really count.
		 * Essentially we want to know about
		 * access beyond that normally done prior to a
		 * bind.
		 */

		switch(call->request->type) {
		case LDAP_TAG_BindRequest:
		case LDAP_TAG_UnbindRequest:
		case LDAP_TAG_AbandonRequest:
			log = false;
			break;
		case LDAP_TAG_ExtendedResponse: {
			struct ldap_ExtendedRequest *req = &call->request->r.ExtendedRequest;
			if (strcmp(req->oid, LDB_EXTENDED_START_TLS_OID) == 0) {
				log = false;
			}
			break;
		}
		case LDAP_TAG_SearchRequest: {
			struct ldap_SearchRequest *req = &call->request->r.SearchRequest;
			if (req->scope == LDAP_SEARCH_SCOPE_BASE) {
				if (req->basedn[0] == '\0') {
					log = false;
				}
			}
			break;
		}
		default:
			break;
		}

		if (log) {
			const char *transport_protection = AUTHZ_TRANSPORT_PROTECTION_NONE;
			if (call->conn->sockets.active == call->conn->sockets.tls) {
				transport_protection = AUTHZ_TRANSPORT_PROTECTION_TLS;
			}

			log_successful_authz_event(call->conn->connection->msg_ctx,
						   call->conn->connection->lp_ctx,
						   call->conn->connection->remote_address,
						   call->conn->connection->local_address,
						   "LDAP",
						   "no bind",
						   transport_protection,
						   call->conn->session_info,
						   NULL /* client_audit_info */,
						   NULL /* server_audit_info */);

			call->conn->authz_logged = true;
		}
	}

	switch(call->request->type) {
	case LDAP_TAG_BindRequest:
		return ldapsrv_BindRequest(call);
	case LDAP_TAG_UnbindRequest:
		return ldapsrv_UnbindRequest(call);
	case LDAP_TAG_SearchRequest:
		return ldapsrv_SearchRequest(call);
	case LDAP_TAG_ModifyRequest:
		status = ldapsrv_ModifyRequest(call);
		break;
	case LDAP_TAG_AddRequest:
		status = ldapsrv_AddRequest(call);
		break;
	case LDAP_TAG_DelRequest:
		status = ldapsrv_DelRequest(call);
		break;
	case LDAP_TAG_ModifyDNRequest:
		status = ldapsrv_ModifyDNRequest(call);
		break;
	case LDAP_TAG_CompareRequest:
		return ldapsrv_CompareRequest(call);
	case LDAP_TAG_AbandonRequest:
		return ldapsrv_AbandonRequest(call);
	case LDAP_TAG_ExtendedRequest:
		status = ldapsrv_ExtendedRequest(call);
		break;
	default:
		return ldapsrv_unwilling(call, LDAP_PROTOCOL_ERROR);
	}

	if (NT_STATUS_IS_OK(status)) {
		ldapsrv_notification_retry_setup(call->conn->service, true);
	}

	return status;
}

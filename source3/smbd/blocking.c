/* 
   Unix SMB/CIFS implementation.
   Blocking Locking functions
   Copyright (C) Jeremy Allison 1998-2003

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
#include "locking/share_mode_lock.h"
#include "smbd/smbd.h"
#include "smbd/globals.h"
#include "messages.h"
#include "lib/util/tevent_ntstatus.h"
#include "lib/dbwrap/dbwrap_watch.h"
#include "librpc/gen_ndr/ndr_open_files.h"

#undef DBGC_CLASS
#define DBGC_CLASS DBGC_LOCKING

NTSTATUS smbd_do_locks_try(struct byte_range_lock *br_lck,
			   struct smbd_do_locks_state *state)
{
	bool unlock_ok;
	uint16_t i;
	NTSTATUS status = NT_STATUS_OK;

	for (i = 0; i < state->num_locks; i++) {
		struct smbd_lock_element *e = &state->locks[i];

		status = do_lock(
			br_lck,
			state->locks, /* req_mem_ctx */
			&e->req_guid,
			e->smblctx,
			e->count,
			e->offset,
			e->brltype,
			e->lock_flav,
			&state->blocking_pid,
			&state->blocking_smblctx);
		if (!NT_STATUS_IS_OK(status)) {
			break;
		}
	}

	if (NT_STATUS_IS_OK(status)) {
		return NT_STATUS_OK;
	}

	state->blocker_idx = i;
	unlock_ok = true;

	/*
	 * Undo the locks we successfully got
	 */
	for (i = i-1; i != UINT16_MAX; i--) {
		struct smbd_lock_element *e = &state->locks[i];
		NTSTATUS ulstatus;

		ulstatus = do_unlock(br_lck,
				     e->smblctx,
				     e->count,
				     e->offset,
				     e->lock_flav);
		if (!NT_STATUS_IS_OK(ulstatus)) {
			DBG_DEBUG("Failed to undo lock flavour %s lock "
				  "type %s start=%"PRIu64" len=%"PRIu64" "
				  "requested for file [%s]\n",
				  lock_flav_name(e->lock_flav),
				  lock_type_name(e->brltype),
				  e->offset,
				  e->count,
				  fsp_str_dbg(brl_fsp(br_lck)));
			unlock_ok = false;
		}
	}
	if (unlock_ok) {
		brl_set_modified(br_lck, false);
	}

	return status;
}

static bool smbd_smb1_fsp_add_blocked_lock_req(
	struct files_struct *fsp, struct tevent_req *req)
{
	size_t num_reqs = talloc_array_length(fsp->blocked_smb1_lock_reqs);
	struct tevent_req **tmp = NULL;

	tmp = talloc_realloc(
		fsp,
		fsp->blocked_smb1_lock_reqs,
		struct tevent_req *,
		num_reqs+1);
	if (tmp == NULL) {
		return false;
	}
	fsp->blocked_smb1_lock_reqs = tmp;
	fsp->blocked_smb1_lock_reqs[num_reqs] = req;
	return true;
}

struct smbd_smb1_do_locks_state {
	struct tevent_context *ev;
	struct smb_request *smbreq;
	struct files_struct *fsp;
	uint32_t timeout;
	uint32_t polling_msecs;
	uint32_t retry_msecs;
	struct timeval endtime;
	bool large_offset;	/* required for correct cancel */
	uint16_t num_locks;
	struct smbd_lock_element *locks;
	uint16_t blocker;
	NTSTATUS deny_status;
};

static void smbd_smb1_do_locks_try(struct tevent_req *req);
static void smbd_smb1_do_locks_retry(struct tevent_req *subreq);
static void smbd_smb1_blocked_locks_cleanup(
	struct tevent_req *req, enum tevent_req_state req_state);

static void smbd_smb1_do_locks_setup_timeout(
	struct smbd_smb1_do_locks_state *state,
	const struct smbd_lock_element *blocker)
{
	struct files_struct *fsp = state->fsp;

	if (!timeval_is_zero(&state->endtime)) {
		/*
		 * already done
		 */
		return;
	}

	if ((state->timeout != 0) && (state->timeout != UINT32_MAX)) {
		/*
		 * Windows internal resolution for blocking locks
		 * seems to be about 200ms... Don't wait for less than
		 * that. JRA.
		 */
		state->timeout = MAX(state->timeout, lp_lock_spin_time());
	}

	if (state->timeout != 0) {
		goto set_endtime;
	}

	if (blocker == NULL) {
		goto set_endtime;
	}

	if ((blocker->offset >= 0xEF000000) &&
	    ((blocker->offset >> 63) == 0)) {
		/*
		 * This must be an optimization of an ancient
		 * application bug...
		 */
		state->timeout = lp_lock_spin_time();
	}

	if (fsp->fsp_flags.lock_failure_seen &&
	    (blocker->offset == fsp->lock_failure_offset)) {
		/*
		 * Delay repeated lock attempts on the same
		 * lock. Maybe a more advanced version of the
		 * above check?
		 */
		DBG_DEBUG("Delaying lock request due to previous "
			  "failure\n");
		state->timeout = lp_lock_spin_time();
	}

set_endtime:
	/*
	 * Note state->timeout might still 0,
	 * but that's ok, as we don't want to retry
	 * in that case.
	 */
	state->endtime = timeval_add(&state->smbreq->request_time,
				     state->timeout / 1000,
				     (state->timeout % 1000) * 1000);
}

static void smbd_smb1_do_locks_update_retry_msecs(
	struct smbd_smb1_do_locks_state *state)
{
	/*
	 * The default lp_lock_spin_time() is 200ms,
	 * we just use half of it to trigger the first retry.
	 *
	 * v_min is in the range of 0.001 to 10 secs
	 * (0.1 secs by default)
	 *
	 * v_max is in the range of 0.01 to 100 secs
	 * (1.0 secs by default)
	 *
	 * The typical steps are:
	 * 0.1, 0.2, 0.3, 0.4, ... 1.0
	 */
	uint32_t v_min = MAX(2, MIN(20000, lp_lock_spin_time()))/2;
	uint32_t v_max = 10 * v_min;

	if (state->retry_msecs >= v_max) {
		state->retry_msecs = v_max;
		return;
	}

	state->retry_msecs += v_min;
}

static void smbd_smb1_do_locks_update_polling_msecs(
	struct smbd_smb1_do_locks_state *state)
{
	/*
	 * The default lp_lock_spin_time() is 200ms.
	 *
	 * v_min is in the range of 0.002 to 20 secs
	 * (0.2 secs by default)
	 *
	 * v_max is in the range of 0.02 to 200 secs
	 * (2.0 secs by default)
	 *
	 * The typical steps are:
	 * 0.2, 0.4, 0.6, 0.8, ... 2.0
	 */
	uint32_t v_min = MAX(2, MIN(20000, lp_lock_spin_time()));
	uint32_t v_max = 10 * v_min;

	if (state->polling_msecs >= v_max) {
		state->polling_msecs = v_max;
		return;
	}

	state->polling_msecs += v_min;
}

struct tevent_req *smbd_smb1_do_locks_send(
	TALLOC_CTX *mem_ctx,
	struct tevent_context *ev,
	struct smb_request **smbreq, /* talloc_move()d into our state */
	struct files_struct *fsp,
	uint32_t lock_timeout,
	bool large_offset,
	uint16_t num_locks,
	struct smbd_lock_element *locks)
{
	struct tevent_req *req = NULL;
	struct smbd_smb1_do_locks_state *state = NULL;
	bool ok;

	req = tevent_req_create(
		mem_ctx, &state, struct smbd_smb1_do_locks_state);
	if (req == NULL) {
		return NULL;
	}
	state->ev = ev;
	state->smbreq = talloc_move(state, smbreq);
	state->fsp = fsp;
	state->timeout = lock_timeout;
	state->large_offset = large_offset;
	state->num_locks = num_locks;
	state->locks = locks;
	state->deny_status = NT_STATUS_LOCK_NOT_GRANTED;

	DBG_DEBUG("state=%p, state->smbreq=%p\n", state, state->smbreq);

	if (num_locks == 0 || locks == NULL) {
		DBG_DEBUG("no locks\n");
		tevent_req_done(req);
		return tevent_req_post(req, ev);
	}

	if (state->locks[0].lock_flav == POSIX_LOCK) {
		/*
		 * SMB1 posix locks always use
		 * NT_STATUS_FILE_LOCK_CONFLICT.
		 */
		state->deny_status = NT_STATUS_FILE_LOCK_CONFLICT;
	}

	smbd_smb1_do_locks_try(req);
	if (!tevent_req_is_in_progress(req)) {
		return tevent_req_post(req, ev);
	}

	ok = smbd_smb1_fsp_add_blocked_lock_req(fsp, req);
	if (!ok) {
		tevent_req_oom(req);
		return tevent_req_post(req, ev);
	}
	tevent_req_set_cleanup_fn(req, smbd_smb1_blocked_locks_cleanup);
	return req;
}

static void smbd_smb1_blocked_locks_cleanup(
	struct tevent_req *req, enum tevent_req_state req_state)
{
	struct smbd_smb1_do_locks_state *state = tevent_req_data(
		req, struct smbd_smb1_do_locks_state);
	struct files_struct *fsp = state->fsp;
	struct tevent_req **blocked = fsp->blocked_smb1_lock_reqs;
	size_t num_blocked = talloc_array_length(blocked);
	size_t i;

	DBG_DEBUG("req=%p, state=%p, req_state=%d\n",
		  req,
		  state,
		  (int)req_state);

	if (req_state == TEVENT_REQ_RECEIVED) {
		DBG_DEBUG("already received\n");
		return;
	}

	for (i=0; i<num_blocked; i++) {
		if (blocked[i] == req) {
			break;
		}
	}
	SMB_ASSERT(i<num_blocked);

	ARRAY_DEL_ELEMENT(blocked, i, num_blocked);

	fsp->blocked_smb1_lock_reqs = talloc_realloc(
		fsp, blocked, struct tevent_req *, num_blocked-1);
}

static NTSTATUS smbd_smb1_do_locks_check_blocked(
	uint16_t num_blocked,
	struct smbd_lock_element *blocked,
	uint16_t num_locks,
	struct smbd_lock_element *locks,
	uint16_t *blocker_idx,
	uint64_t *blocking_smblctx)
{
	uint16_t li;

	for (li=0; li < num_locks; li++) {
		struct smbd_lock_element *l = &locks[li];
		uint16_t bi;
		bool valid;

		valid = byte_range_valid(l->offset, l->count);
		if (!valid) {
			return NT_STATUS_INVALID_LOCK_RANGE;
		}

		for (bi = 0; bi < num_blocked; bi++) {
			struct smbd_lock_element *b = &blocked[li];
			bool overlap;

			/* Read locks never conflict. */
			if (l->brltype == READ_LOCK && b->brltype == READ_LOCK) {
				continue;
			}

			overlap = byte_range_overlap(l->offset,
						     l->count,
						     b->offset,
						     b->count);
			if (!overlap) {
				continue;
			}

			*blocker_idx = li;
			*blocking_smblctx = b->smblctx;
			return NT_STATUS_LOCK_NOT_GRANTED;
		}
	}

	return NT_STATUS_OK;
}

static void smbd_smb1_do_locks_try_fn(struct share_mode_lock *lck,
				      struct byte_range_lock *br_lck,
				      void *private_data)
{
	struct tevent_req *req = talloc_get_type_abort(
		private_data, struct tevent_req);
	struct smbd_smb1_do_locks_state *state = tevent_req_data(
		req, struct smbd_smb1_do_locks_state);
	struct smbd_do_locks_state brl_state;
	struct files_struct *fsp = state->fsp;
	struct tevent_req **blocked = fsp->blocked_smb1_lock_reqs;
	size_t num_blocked = talloc_array_length(blocked);
	struct timeval endtime = { 0 };
	struct tevent_req *subreq = NULL;
	size_t bi;
	NTSTATUS status;
	bool ok;
	bool expired;

	/*
	 * The caller has checked fsp->fsp_flags.can_lock and lp_locking so
	 * br_lck has to be there!
	 */
	SMB_ASSERT(br_lck != NULL);

	brl_state = (struct smbd_do_locks_state) {
		.num_locks = state->num_locks,
		.locks = state->locks,
	};

	/*
	 * We check the pending/blocked requests
	 * from the oldest to the youngest request.
	 *
	 * Note due to the retry logic the current request
	 * might already be in the list.
	 */

	for (bi = 0; bi < num_blocked; bi++) {
		struct smbd_smb1_do_locks_state *blocked_state =
			tevent_req_data(blocked[bi],
			struct smbd_smb1_do_locks_state);

		if (blocked_state->locks == state->locks) {
			SMB_ASSERT(blocked_state->num_locks == state->num_locks);

			/*
			 * We found ourself...
			 */
			break;
		}

		status = smbd_smb1_do_locks_check_blocked(
				blocked_state->num_locks,
				blocked_state->locks,
				state->num_locks,
				state->locks,
				&brl_state.blocker_idx,
				&brl_state.blocking_smblctx);
		if (!NT_STATUS_IS_OK(status)) {
			brl_state.blocking_pid = messaging_server_id(
				fsp->conn->sconn->msg_ctx);
			goto check_retry;
		}
	}

	status = smbd_do_locks_try(br_lck, &brl_state);
	if (NT_STATUS_IS_OK(status)) {
		goto done;
	}

	state->blocker = brl_state.blocker_idx;

	if (NT_STATUS_EQUAL(status, NT_STATUS_RETRY)) {
		/*
		 * We got NT_STATUS_RETRY,
		 * we reset polling_msecs so that
		 * that the retries based on LOCK_NOT_GRANTED
		 * will later start with small intervals again.
		 */
		state->polling_msecs = 0;

		/*
		 * The backend wasn't able to decide yet.
		 * We need to wait even for non-blocking
		 * locks.
		 *
		 * The backend uses blocking_smblctx == UINT64_MAX
		 * to indicate that we should use retry timers.
		 *
		 * It uses blocking_smblctx == 0 to indicate
		 * it will use share_mode_wakeup_waiters()
		 * to wake us. Note that unrelated changes in
		 * locking.tdb may cause retries.
		 */

		if (brl_state.blocking_smblctx != UINT64_MAX) {
			SMB_ASSERT(brl_state.blocking_smblctx == 0);
			goto setup_retry;
		}

		smbd_smb1_do_locks_update_retry_msecs(state);

		DBG_DEBUG("Waiting for a backend decision. "
			  "Retry in %"PRIu32" msecs\n",
			  state->retry_msecs);

		/*
		 * We completely ignore state->endtime here
		 * we we'll wait for a backend decision forever.
		 * If the backend is smart enough to implement
		 * some NT_STATUS_RETRY logic, it has to
		 * switch to any other status after in order
		 * to avoid waiting forever.
		 */
		endtime = timeval_current_ofs_msec(state->retry_msecs);
		goto setup_retry;
	}

check_retry:
	if (!ERROR_WAS_LOCK_DENIED(status)) {
		goto done;
	}
	/*
	 * We got LOCK_NOT_GRANTED, make sure
	 * a following STATUS_RETRY will start
	 * with short intervals again.
	 */
	state->retry_msecs = 0;

	smbd_smb1_do_locks_setup_timeout(state, &state->locks[state->blocker]);
	DBG_DEBUG("timeout=%"PRIu32", blocking_smblctx=%"PRIu64"\n",
		  state->timeout,
		  brl_state.blocking_smblctx);

	/*
	 * The client specified timeout expired
	 * avoid further retries.
	 *
	 * Otherwise keep waiting either waiting
	 * for changes in locking.tdb or the polling
	 * mode timers waiting for posix locks.
	 *
	 * If the endtime is not expired yet,
	 * it means we'll retry after a timeout.
	 * In that case we'll have to return
	 * NT_STATUS_FILE_LOCK_CONFLICT
	 * instead of NT_STATUS_LOCK_NOT_GRANTED.
	 */
	expired = timeval_expired(&state->endtime);
	if (expired) {
		status = state->deny_status;
		goto done;
	}
	state->deny_status = NT_STATUS_FILE_LOCK_CONFLICT;

	endtime = state->endtime;

	if (brl_state.blocking_smblctx == UINT64_MAX) {
		struct timeval tmp;

		smbd_smb1_do_locks_update_polling_msecs(state);

		DBG_DEBUG("Blocked on a posix lock. Retry in %"PRIu32" msecs\n",
			  state->polling_msecs);

		tmp = timeval_current_ofs_msec(state->polling_msecs);
		endtime = timeval_min(&endtime, &tmp);
	}

setup_retry:
	subreq = share_mode_watch_send(
		state, state->ev, &state->fsp->file_id, brl_state.blocking_pid);
	if (tevent_req_nomem(subreq, req)) {
		status = NT_STATUS_NO_MEMORY;
		goto done;
	}
	tevent_req_set_callback(subreq, smbd_smb1_do_locks_retry, req);

	if (timeval_is_zero(&endtime)) {
		return;
	}

	ok = tevent_req_set_endtime(subreq, state->ev, endtime);
	if (!ok) {
		status = NT_STATUS_NO_MEMORY;
		goto done;
	}
	return;
done:
	smbd_smb1_brl_finish_by_req(req, status);
}

static void smbd_smb1_do_locks_try(struct tevent_req *req)
{
	struct smbd_smb1_do_locks_state *state = tevent_req_data(
		req, struct smbd_smb1_do_locks_state);
	NTSTATUS status;

	if (!state->fsp->fsp_flags.can_lock) {
		if (state->fsp->fsp_flags.is_directory) {
			return smbd_smb1_brl_finish_by_req(req,
					NT_STATUS_INVALID_DEVICE_REQUEST);
		}
		return smbd_smb1_brl_finish_by_req(req,
						   NT_STATUS_INVALID_HANDLE);
	}

	if (!lp_locking(state->fsp->conn->params)) {
		return smbd_smb1_brl_finish_by_req(req, NT_STATUS_OK);
	}

	status = share_mode_do_locked_brl(state->fsp,
					  smbd_smb1_do_locks_try_fn,
					  req);
	if (!NT_STATUS_IS_OK(status)) {
		smbd_smb1_brl_finish_by_req(req, status);
		return;
	}
	return;
}

static void smbd_smb1_do_locks_retry(struct tevent_req *subreq)
{
	struct tevent_req *req = tevent_req_callback_data(
		subreq, struct tevent_req);
	struct smbd_smb1_do_locks_state *state = tevent_req_data(
		req, struct smbd_smb1_do_locks_state);
	NTSTATUS status;
	bool ok;

	/*
	 * Make sure we run as the user again
	 */
	ok = change_to_user_and_service_by_fsp(state->fsp);
	if (!ok) {
		tevent_req_nterror(req, NT_STATUS_ACCESS_DENIED);
		return;
	}

	status = share_mode_watch_recv(subreq, NULL, NULL);
	TALLOC_FREE(subreq);

	DBG_DEBUG("share_mode_watch_recv returned %s\n",
		  nt_errstr(status));

	/*
	 * We ignore any errors here, it's most likely
	 * we just get NT_STATUS_OK or NT_STATUS_IO_TIMEOUT.
	 *
	 * In any case we can just give it a retry.
	 */

	smbd_smb1_do_locks_try(req);
}

NTSTATUS smbd_smb1_do_locks_recv(struct tevent_req *req)
{
	struct smbd_smb1_do_locks_state *state = tevent_req_data(
		req, struct smbd_smb1_do_locks_state);
	NTSTATUS status = NT_STATUS_OK;
	bool err;

	err = tevent_req_is_nterror(req, &status);

	DBG_DEBUG("err=%d, status=%s\n", (int)err, nt_errstr(status));

	if (tevent_req_is_nterror(req, &status)) {
		struct files_struct *fsp = state->fsp;
		struct smbd_lock_element *blocker =
			&state->locks[state->blocker];

		DBG_DEBUG("Setting lock_failure_offset=%"PRIu64"\n",
			  blocker->offset);

		fsp->fsp_flags.lock_failure_seen = true;
		fsp->lock_failure_offset = blocker->offset;
		return status;
	}

	tevent_req_received(req);

	return NT_STATUS_OK;
}

bool smbd_smb1_do_locks_extract_smbreq(
	struct tevent_req *req,
	TALLOC_CTX *mem_ctx,
	struct smb_request **psmbreq)
{
	struct smbd_smb1_do_locks_state *state = tevent_req_data(
		req, struct smbd_smb1_do_locks_state);

	DBG_DEBUG("req=%p, state=%p, state->smbreq=%p\n",
		  req,
		  state,
		  state->smbreq);

	if (state->smbreq == NULL) {
		return false;
	}
	*psmbreq = talloc_move(mem_ctx, &state->smbreq);
	return true;
}

void smbd_smb1_brl_finish_by_req(struct tevent_req *req, NTSTATUS status)
{
	DBG_DEBUG("req=%p, status=%s\n", req, nt_errstr(status));

	if (NT_STATUS_IS_OK(status)) {
		tevent_req_done(req);
	} else {
		tevent_req_nterror(req, status);
	}
}

bool smbd_smb1_brl_finish_by_lock(
	struct files_struct *fsp,
	bool large_offset,
	struct smbd_lock_element lock,
	NTSTATUS finish_status)
{
	struct tevent_req **blocked = fsp->blocked_smb1_lock_reqs;
	size_t num_blocked = talloc_array_length(blocked);
	size_t i;

	DBG_DEBUG("num_blocked=%zu\n", num_blocked);

	for (i=0; i<num_blocked; i++) {
		struct tevent_req *req = blocked[i];
		struct smbd_smb1_do_locks_state *state = tevent_req_data(
			req, struct smbd_smb1_do_locks_state);
		uint16_t j;

		DBG_DEBUG("i=%zu, req=%p\n", i, req);

		if (state->large_offset != large_offset) {
			continue;
		}

		for (j=0; j<state->num_locks; j++) {
			struct smbd_lock_element *l = &state->locks[j];

			if ((lock.smblctx == l->smblctx) &&
			    (lock.offset == l->offset) &&
			    (lock.count == l->count)) {
				smbd_smb1_brl_finish_by_req(
					req, finish_status);
				return true;
			}
		}
	}
	return false;
}

static struct files_struct *smbd_smb1_brl_finish_by_mid_fn(
	struct files_struct *fsp, void *private_data)
{
	struct tevent_req **blocked = fsp->blocked_smb1_lock_reqs;
	size_t num_blocked = talloc_array_length(blocked);
	uint64_t mid = *((uint64_t *)private_data);
	size_t i;

	DBG_DEBUG("fsp=%p, num_blocked=%zu\n", fsp, num_blocked);

	for (i=0; i<num_blocked; i++) {
		struct tevent_req *req = blocked[i];
		struct smbd_smb1_do_locks_state *state = tevent_req_data(
			req, struct smbd_smb1_do_locks_state);
		struct smb_request *smbreq = state->smbreq;

		if (smbreq->mid == mid) {
			tevent_req_nterror(req, NT_STATUS_FILE_LOCK_CONFLICT);
			return fsp;
		}
	}

	return NULL;
}

/*
 * This walks the list of fsps, we store the blocked reqs attached to
 * them. It can be expensive, but this is legacy SMB1 and trying to
 * remember looking at traces I don't really see many of those calls.
 */

bool smbd_smb1_brl_finish_by_mid(
	struct smbd_server_connection *sconn, uint64_t mid)
{
	struct files_struct *found = files_forall(
		sconn, smbd_smb1_brl_finish_by_mid_fn, &mid);
	return (found != NULL);
}

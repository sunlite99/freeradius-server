/*
 *   This program is free software; you can redistribute it and/or modify
 *   it under the terms of the GNU General Public License as published by
 *   the Free Software Foundation; either version 2 of the License, or
 *   (at your option) any later version.
 *
 *   This program is distributed in the hope that it will be useful,
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *   GNU General Public License for more details.
 *
 *   You should have received a copy of the GNU General Public License
 *   along with this program; if not, write to the Free Software
 *   Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301, USA
 */

/**
 * $Id$
 *
 * @file unlang/interpret_synchronous.c
 * @brief Synchronous interpreter
 *
 * @copyright 2021 The FreeRADIUS server project
 */

#include "interpret_priv.h"
#include <freeradius-devel/server/module_rlm.h>

typedef struct {
	fr_heap_t		*runnable;
	fr_event_list_t		*el;
	int			yielded;
	unlang_interpret_t	*intp;
} unlang_interpret_synchronous_t;

/** Internal request (i.e. one generated by the interpreter) is now complete
 *
 */
static void _request_init_internal(request_t *request, void *uctx)
{
	unlang_interpret_synchronous_t	*intps = uctx;

	RDEBUG3("Initialising internal synchronous request");
	unlang_interpret_set(request, intps->intp);
	fr_heap_insert(&intps->runnable, request);
}

/** External request is now complete
 *
 */
static void _request_done_external(request_t *request, UNUSED rlm_rcode_t rcode, UNUSED void *uctx)
{
	if (request->master_state != REQUEST_STOP_PROCESSING) {
		/*
		 *	If we're running a real request, then the final
		 *	indentation MUST be zero.  Otherwise we skipped
		 *	something!
		 *
		 *	Also check that the request is NOT marked as
		 *	"yielded", but is in fact done.
		 *
		 *	@todo - check that the stack is at frame 0, otherwise
		 *	more things have gone wrong.
		 */
		fr_assert_msg(request->parent || (request->log.unlang_indent == 0),
			      "Request %s bad log indentation - expected 0 got %u", request->name, request->log.unlang_indent);
		fr_assert_msg(!unlang_interpret_is_resumable(request),
			      "Request %s is marked as yielded at end of processing", request->name);
	}

	RDEBUG3("Synchronous done external request");
}

/** Internal request (i.e. one generated by the interpreter) is now complete
 *
 */
static void _request_done_internal(request_t *request, UNUSED rlm_rcode_t rcode, UNUSED void *uctx)
{
	RDEBUG3("Done synchronous internal request");

	/* Request will be cleaned up by whatever created it */
}

static void _request_done_detached(request_t *request, UNUSED rlm_rcode_t rcode, UNUSED void *uctx)
{
	RDEBUG3("Done synchronous detached request");
}

/** We don't need to do anything for internal -> detached
 *
 */
static void _request_detach(request_t *request, UNUSED void *uctx)
{
	RDEBUG3("Synchronous request detached");
}

/** Request has been stopped
 *
 * Clean up execution state
 */
static void _request_stop(request_t *request, void *uctx)
{
	unlang_interpret_synchronous_t	*intps = uctx;

	RDEBUG3("Stopped detached request");

	fr_heap_extract(&intps->runnable, request);
}

/** Request is now runnable
 *
 */
static void _request_runnable(request_t *request, void *uctx)
{
	unlang_interpret_synchronous_t	*intps = uctx;

	fr_assert(intps->yielded > 0);
	intps->yielded--;

	fr_heap_insert(&intps->runnable, request);
}

/** Interpreter yielded request
 *
 */
static void _request_yield(request_t *request, void *uctx)
{
	unlang_interpret_synchronous_t	*intps = uctx;

	intps->yielded++;

	RDEBUG3("synchronous request yielded");
}

/** Interpreter is starting to work on request again
 *
 */
static void _request_resume(request_t *request, UNUSED void *uctx)
{
	RDEBUG3("synchronous request resumed");
}

static bool _request_scheduled(request_t const *request, UNUSED void *uctx)
{
	return fr_heap_entry_inserted(request->runnable_id);
}

/** Allocate a new temporary interpreter
 *
 */
static unlang_interpret_synchronous_t *unlang_interpret_synchronous_alloc(TALLOC_CTX *ctx, fr_event_list_t *el)
{
	unlang_interpret_synchronous_t *intps;

	MEM(intps = talloc_zero(ctx, unlang_interpret_synchronous_t));
	MEM(intps->runnable = fr_heap_talloc_alloc(intps, fr_pointer_cmp, request_t, runnable_id, 0));
	if (el) {
		intps->el = el;
	} else {
		MEM(intps->el = fr_event_list_alloc(intps, NULL, NULL));
	}
	MEM(intps->intp = unlang_interpret_init(intps, intps->el,
						&(unlang_request_func_t){
							.init_internal = _request_init_internal,

							.done_external = _request_done_external,
							.done_internal = _request_done_internal,
							.done_detached = _request_done_detached,

							.detach = _request_detach,
							.stop = _request_stop,
							.yield = _request_yield,
							.resume = _request_resume,
							.mark_runnable = _request_runnable,
							.scheduled = _request_scheduled
						},
						intps));

	return intps;
}

/** Execute an unlang section synchronously
 *
 * Create a temporary event loop and swap it out for the one in the request.
 * Execute unlang operations until we receive a non-yield return code then return.
 *
 * @note The use cases for this are very limited.  If you need to use it, chances
 *	are what you're doing could be done better using one of the thread
 *	event loops.
 *
 * @param[in] request	The current request.
 * @return One of the RLM_MODULE_* macros.
 */
rlm_rcode_t unlang_interpret_synchronous(fr_event_list_t *el, request_t *request)
{
	unlang_interpret_t 		*old_intp;
	char const			*caller;

	unlang_interpret_synchronous_t	*intps;

	rlm_rcode_t			rcode;

	request_t			*sub_request = NULL;
	bool				dont_wait_for_event;
	int				iterations = 0;

	old_intp = unlang_interpret_get(request);
	caller = request->module;

	intps = unlang_interpret_synchronous_alloc(request, el);
	unlang_interpret_set(request, intps->intp);

	el = intps->el;

	rcode = unlang_interpret(request);

	while ((dont_wait_for_event = (fr_heap_num_elements(intps->runnable) > 0)) ||
	       (intps->yielded > 0)) {
		rlm_rcode_t	sub_rcode;
		int		num_events;

		/*
		 *	Wait for a timer / IO event.  If there's a
		 *	failure, all kinds of bad things happen.  Oh
		 *	well.
		 */
		DEBUG3("Gathering events - %s", dont_wait_for_event ? "Will not wait" : "will wait");
		num_events = fr_event_corral(el, fr_time(), !dont_wait_for_event);
		if (num_events < 0) {
			RPERROR("fr_event_corral");
			break;
		}

		DEBUG3("%u event(s) pending%s",
		       num_events == -1 ? 0 : num_events, num_events == -1 ? " - event loop exiting" : "");

		/*
		 *	This function ends up pushing a
		 *	runnable request into the backlog, OR
		 *	setting new timers.
		 */
		if (num_events > 0) {
			DEBUG4("Servicing event(s)");
			fr_event_service(el);
		}

		/*
		 *	If there are no runnable requests, then go
		 *	back to check the timers again.  Note that we
		 *	only wait if there are timer events left to
		 *	service.
		 *
		 *	If there WAS a timer event, but servicing that
		 *	timer event did not result in a runnable
		 *	request, THEN we're guaranteed that there is
		 *	still a timer event left.
		 */
		sub_request = fr_heap_pop(&intps->runnable);
		if (!sub_request) {
			DEBUG3("No pending requests (%u yielded)", intps->yielded);
			continue;
		}

		/*
		 *	Continue interpretation until there's nothing
		 *	in the backlog.  If this request YIELDs, then
		 *	do another loop around.
		 */
		RDEBUG4(">>> interpreter (iteration %i)", ++iterations);
		sub_rcode = unlang_interpret(sub_request);
		RDEBUG4("<<< interpreter (iteration %i) - %s", iterations,
			fr_table_str_by_value(rcode_table, sub_rcode, "<INVALID>"));

		if (sub_request == request) {
			rcode = sub_rcode;
		/*
		 *	Free detached, resumable requests
		 */
		} else if (!sub_request->parent && !unlang_interpret_is_resumable(sub_request)) {
			talloc_free(sub_request);	/* Free detached requests */
		}

		DEBUG3("%u runnable, %u yielded", fr_heap_num_elements(intps->runnable), intps->yielded);
	}

	talloc_free(intps);
	unlang_interpret_set(request, old_intp);
	request->module = caller;

	return rcode;
}

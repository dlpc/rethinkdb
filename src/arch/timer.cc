// Copyright 2010-2012 RethinkDB, all rights reserved.
#include "arch/timer.hpp"
#include "utils.hpp"

timer_handler_t::timer_handler_t(linux_event_queue_t *queue)
    : timer_provider(queue),
      expected_oneshot_time_in_nanos(0) {
    // Right now, we have no tokens.  So we don't ask the timer provider to do anything for us.
}

timer_handler_t::~timer_handler_t() {
    guarantee(token_queue.empty());
}

void timer_handler_t::on_oneshot() {
    // If the timer_provider tends to return its callback a touch early, we don't want to make a
    // bunch of calls to it, returning a tad early over and over again, leading up to a ticks
    // threshold.  So we bump the real time up to the threshold when processing the priority queue.
    int64_t real_ticks = get_ticks();
    int64_t ticks = std::max(real_ticks, expected_oneshot_time_in_nanos);

    while (!token_queue.empty() && token_queue.peek()->next_time_in_nanos <= ticks) {
        timer_token_t *token = token_queue.pop();

        // Put the repeating timer back on the queue before the callback can be called (so that it
        // may be canceled).
        if (token->interval_nanos != 0) {
            token->next_time_in_nanos = real_ticks + token->interval_nanos;
            token_queue.push(token);
        }

        token->callback->on_timer();

        // Delete nonrepeating timer tokens.
        if (token->interval_nanos == 0) {
            delete token;
        }
    }

    // We've processed young tokens.  Now schedule a new one-shot (if necessary).
    if (!token_queue.empty()) {
        timer_provider.schedule_oneshot(token_queue.peek()->next_time_in_nanos, this);
    }
}

timer_token_t *timer_handler_t::add_timer_internal(const int64_t ms, timer_callback_t *callback, const bool once) {
    const int64_t nanos = ms * 1000000LL;
    guarantee(nanos > 0);

    const int64_t next_time_in_nanos = get_ticks() + nanos;

    timer_token_t *const token = new timer_token_t;
    token->interval_nanos = once ? 0 : nanos;
    token->next_time_in_nanos = next_time_in_nanos;
    token->callback = callback;

    const timer_token_t *top_entry = token_queue.peek();
    token_queue.push(token);

    if (top_entry == NULL || next_time_in_nanos < top_entry->next_time_in_nanos) {
        timer_provider.schedule_oneshot(next_time_in_nanos, this);
    }

    return token;
}

void timer_handler_t::cancel_timer(timer_token_t *token) {
    rassert(token->interval_nanos != 0, "This is not necessarily a bug, tell Sam if you see this.");

    token_queue.remove(token);

    if (token_queue.empty()) {
        timer_provider.unschedule_oneshot();
    }
}

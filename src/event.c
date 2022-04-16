/*
 * Copyright (c) 2016-2020 David Leeds <davidesleeds@gmail.com>
 *
 * Event is free software; you can redistribute it and/or modify
 * it under the terms of the MIT license. See LICENSE for details.
 */

#include <time.h>
#include <unistd.h>
#include <string.h>
#include <fcntl.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/epoll.h>
#include <errno.h>

#if defined(EVENT_LIBSYSTEMD)
#include <systemd/sd-event.h>
#endif /* EVENT_LIBSYSTEMD */

#include <event.h>

#ifndef EVENT_NOASSERT
#include <assert.h>
#define EVENT_ASSERT(expr)	assert(expr)
#else
#define EVENT_ASSERT(expr)
#endif

#define EVENT_EPOLL_READ                (EPOLLIN | EPOLLPRI)
#define EVENT_EPOLL_WRITE               (EPOLLOUT)
#define EVENT_EPOLL_HANGUP              (EPOLLERR | EPOLLHUP | EPOLLRDHUP)
#define EVENT_EPOLL_PRIORITY            (EPOLLPRI)

/*
 * Maximum number of events to process at a time with event_poll().  Remaining
 * events will be processed by a subsequent call.
 */
#define EVENT_MAXEVENTS                 64

/*
 * State for a dispatch event.
 */
struct event_dispatch {
    void (*handler)(void *);
    void *handler_arg;
};


/*
 * Convert an event type mask to epoll events.
 */
static uint32_t event_mask_to_epoll_events(uint32_t events)
{
    uint32_t mask = 0;

    if (events & EVENT_IO_READ) {
        mask |= EVENT_EPOLL_READ;
    }
    if (events & EVENT_IO_WRITE) {
        mask |= EVENT_EPOLL_WRITE;
    }
    if (events & EVENT_IO_DISCONNECT) {
        mask |= EVENT_EPOLL_HANGUP;
    }
    if (events & EVENT_IO_PRIORITY) {
        mask |= EVENT_EPOLL_PRIORITY;
    }
    return mask;
}

/*
 * Convert epoll events to an event type mask.
 */
static uint32_t event_mask_from_epoll_events(uint32_t events)
{
    uint32_t mask = 0;

    if (events & EVENT_EPOLL_READ) {
        mask |= EVENT_IO_READ;
    }
    if (events & EVENT_EPOLL_WRITE) {
        mask |= EVENT_IO_WRITE;
    }
    if (events & EVENT_EPOLL_HANGUP) {
        mask |= EVENT_IO_DISCONNECT;
    }
    if (events & EVENT_EPOLL_PRIORITY) {
        mask |= EVENT_IO_PRIORITY;
    }
    return mask;
}

/*
 * Set a flag to allow event_run() to return on the next event loop iteration.
 */
static void event_stop_handler(void *arg)
{
    struct event_context *ctx = (struct event_context *)arg;

#if defined(EVENT_LIBSYSTEMD)
    if (ctx->sd_event) {
        /* Exit attached sd-event loop instead of stopping our own */
        sd_event_exit(ctx->sd_event, 0);
        return;
    }
#endif /* EVENT_LIBSYSTEMD */

    ctx->stop = true;
}

/*
 * Handle a dispatched callback on the event thread.
 */
static void event_dispatch_handler(struct event_io *io, uint32_t event_mask,
        void *arg)
{
    struct event_dispatch dispatch;

    while (recv(io->fd, &dispatch, sizeof(dispatch), 0) == sizeof(dispatch)) {
        dispatch.handler(dispatch.handler_arg);
    }
}

/*
 * Register the event state to receive dispatched callbacks.
 * Returns 0 on success, or -errno on failure.
 */
static int event_register_dispatch_handler(struct event_context *ctx)
{
    int fds[2];
    int r;

    event_io_init(ctx, &ctx->dispatch_listener, event_dispatch_handler, NULL);

    if (socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0, fds) < 0) {
        return -errno;
    }

    /* Set the receiving file descriptor to non-blocking mode */
    fcntl(fds[1], F_SETFL, fcntl(fds[1], F_GETFL, 0) | O_NONBLOCK);

    /* Listen for dispatch messages */
    r = event_io_register(&ctx->dispatch_listener, fds[1], EVENT_IO_READ);
    if (r < 0) {
        close(fds[0]);
        close(fds[1]);
        return r;
    }
    ctx->dispatch_fd = fds[0];
    return 0;
}

/*
 * Cleanup the dispatch handler.
 */
static void event_unregister_dispatch_handler(struct event_context *ctx)
{
    if (ctx->dispatch_fd == -1) {
        return;
    }

    close(ctx->dispatch_fd);
    ctx->dispatch_fd = -1;

    event_io_close(&ctx->dispatch_listener);
}

/*
 * Applies a new event mask to an I/O listener.  This immediately clears any
 * pending events received from epoll_wait() for this I/O listener that are no
 * longer registered for.  This is needed to safely apply a registration change
 * while processing pending events.
 */
static void event_io_set_mask(struct event_io *io, uint32_t event_mask)
{
    struct epoll_event *e;

    io->event_mask = event_mask;

    if (io->ctx->epoll_pending) {
        for (e = io->ctx->epoll_pending;
                e < &io->ctx->epoll_pending[io->ctx->epoll_pending_len]; ++e) {
            if (e->data.ptr == io) {
                e->events &= event_mask;
            }
        }
    }
}

/*
 * Insert a node into the timer list in chronological order for the specified
 * timeout time.
 */
static void event_timer_insert(struct event_timer *t, uint64_t time_ms)
{
    struct event_timer **prev;
    struct event_timer *node;

    t->time_ms = time_ms;

    /*
     * An elegant sorted insert is beyond the capabilities of the standard
     * sys/queue.h macros, so it is being done manually, here.
     */
    for (prev = &LIST_FIRST(&t->ctx->timer_list); (node = *prev) != NULL;
            prev = &LIST_NEXT(node, entry)) {
        if (t->time_ms < node->time_ms) {
            break;
        }
    }
    /* Doubly linked list insert before node */
    *prev = t;
    t->entry.le_prev = prev;
    t->entry.le_next = node;
    if (node) {
        node->entry.le_prev = &t->entry.le_next;
    }
}

/*
 * Evaluate all active timers.  This may be called at any time, but it is
 * optimal to call it once per scheduled timeout.
 * The return value is the number of milliseconds until the next timer event,
 * or -1 if no timeouts are scheduled.
 */
static int64_t event_timer_poll(struct event_context *ctx)
{
    struct event_timer *t;
    uint64_t cur_time_ms = 0;
    uint64_t next_time_ms;

    EVENT_ASSERT(ctx != NULL);

    while ((t = LIST_FIRST(&ctx->timer_list)) !=  NULL) {
        /*
         * Return if the next timeout is in the future, but re-check
         * the current time in case of long handler run time.
         */
        if (t->time_ms > cur_time_ms) {
            cur_time_ms = event_monotonic_ms();
            if (t->time_ms > cur_time_ms) {
                return t->time_ms - cur_time_ms;
            }
        }

        /* Pop first timer off queue and fire it */
        LIST_REMOVE(t, entry);

        if (t->repeat_ms) {
            /* Periodic timer: schedule next event */
            next_time_ms = t->time_ms;
            do {
                /* Drop any timer repeats that occur in the past */
                next_time_ms += t->repeat_ms;
            } while (next_time_ms <= cur_time_ms);

            event_timer_insert(t, next_time_ms);
        } else {
            /* One-shot timer: clear it */
            t->time_ms = 0;
        }

        t->handler(t, t->handler_arg);
    }
    return -1;
}

/*
 * Block waiting for the next I/O event or timer timeout.
 * Returns 0 on a normal event, signal interrupt, or timeout, and -errno
 * for an unrecoverable error.
 */
static int event_poll(struct event_context *ctx, int timeout_ms)
{
    struct epoll_event epoll_buf[EVENT_MAXEVENTS];
    struct epoll_event *e;
    struct event_io *io;
    uint32_t event_mask;
    int r;

    EVENT_ASSERT(ctx != NULL);

    r = epoll_wait(ctx->epoll_fd, epoll_buf, EVENT_MAXEVENTS, timeout_ms);
    if (r < 0 && errno != EINTR) {
        /* Unexpected error */
        return -errno;
    }

    if (r > 0) {
        /* One or more events to process */
        for (e = epoll_buf; e < &epoll_buf[r]; ++e) {
            io = e->data.ptr;
            event_mask = event_mask_from_epoll_events(e->events);
            if (!event_mask) {
                /* Received an invalidated or unsupported event */
                continue;
            }

            /* Expose pending list for event_io_set_mask() */
            ctx->epoll_pending = e + 1;
            ctx->epoll_pending_len = &epoll_buf[r] - e - 1;

            if (event_mask == EVENT_IO_DISCONNECT &&
                    !(io->event_mask & EVENT_IO_DISCONNECT)) {
                /*
                 * epoll automatically registers listeners for HUP and ERR
                 * events that indicate the I/O endpoint was closed. This
                 * handles the case where the endpoint was closed and there are
                 * no other pending READ/WRITE events, but a DISCONNECT
                 * listener was not registered. To avoid receiving this event
                 * repeatedly, clear this listener now.
                 */
                epoll_ctl(ctx->epoll_fd, EPOLL_CTL_DEL, io->fd, e);
                event_io_set_mask(io, 0);
                continue;
            }

            io->handler(io, event_mask, io->handler_arg);
        }

        ctx->epoll_pending = NULL;
        ctx->epoll_pending_len = 0;
        return 0;
    }

    /* Timeout or signal interruption */
    return 0;
}

/*
 * Initialize an event context.
 * Returns 0 on success, or -errno on failure.
 */
int event_init(struct event_context *ctx)
{
    int r;

    EVENT_ASSERT(ctx != NULL);

    memset(ctx, 0, sizeof(*ctx));

    /* Identify the event thread to detect actions from other threads */
    ctx->thread = pthread_self();

    /* Initialize I/O listener */
    ctx->epoll_fd = epoll_create1(EPOLL_CLOEXEC);
    if (ctx->epoll_fd < 0) {
        return -errno;
    }

    LIST_INIT(&ctx->io_list);
    LIST_INIT(&ctx->timer_list);

    /* Setup thread-safe poll signaling mechanism */
    ctx->dispatch_fd = -1;
    r = event_register_dispatch_handler(ctx);
    if (r < 0) {
        return r;
    }

    return 0;
}

/*
 * Free resources associated with an event context.  Any remaining registered
 * event handlers are unregistered automatically.
 */
void event_cleanup(struct event_context *ctx)
{
    struct event_io *io;
    struct event_timer *t;

    EVENT_ASSERT(ctx != NULL);

    /* Close dispatch sockets */
    event_unregister_dispatch_handler(ctx);

    /* Cancel any active timers */
    while ((t = LIST_FIRST(&ctx->timer_list)) != NULL) {
        event_timer_cancel(t);
    }

    /* Unregister file event listeners */
    while ((io = LIST_FIRST(&ctx->io_list)) != NULL) {
        event_io_unregister(io);
    }

#if defined(EVENT_LIBSYSTEMD)
    /* Cleanup sd-event handlers, if attached */
    event_detach_sdevent(ctx);
#endif /* EVENT_LIBSYSTEMD */

    close(ctx->epoll_fd);
}

/*
 * Start the event loop.  This function blocks indefinitely until event_stop()
 * is called or an unrecoverable error occurs.
 * Returns -errno on error, and 0 on a normal exit.
 */
int event_run(struct event_context *ctx)
{
    int r;

    EVENT_ASSERT(ctx != NULL);

    /* Update in case event_init() was called on a different thread */
    ctx->thread = pthread_self();

#if defined(EVENT_LIBSYSTEMD)
    if (ctx->sd_event) {
        /* Enter attached sd-event loop instead of entering our own */
        return sd_event_loop(ctx->sd_event);
    }
#endif /* EVENT_LIBSYSTEMD */

    /* Event loop */
    while (!ctx->stop) {
        r = event_poll(ctx, event_timer_poll(ctx));
        if (r < 0) {
            return r;
        }
    }
    /* Clear stop flag in to allow another call to event_run(), if needed */
    ctx->stop = false;
    return 0;
}

/*
 * Thread and signal-safe mechanism to signal event_run() to return.
 * Returns 0 on success, or -errno on failure.
 */
int event_stop(struct event_context *ctx)
{
    EVENT_ASSERT(ctx != NULL);

    return event_dispatch(ctx, event_stop_handler, ctx);
}

/*
 * Thread and signal-safe mechanism to invoke a function on the event thread,
 * Returns 0 on success, or -errno on failure.
 */
int event_dispatch(const struct event_context *ctx,
        void (*handler)(void *), void *arg)
{
    struct event_dispatch dispatch = { handler, arg };
    int flags = MSG_NOSIGNAL;

    EVENT_ASSERT(ctx != NULL);
    EVENT_ASSERT(handler != NULL);

    /*
     * Queue a callback to be executed by the dispatch handler on the main loop
     * using a local socket, which is thread and signal-safe. If the event
     * thread is dispatching its own callbacks, send in non-blocking mode to
     * fail if the socket buffer is full, instead of deadlocking the event
     * thread.  Other threads will block if they dispatch more callbacks than
     * the dispatch socket buffer can accept.
     */
    if (pthread_equal(ctx->thread, pthread_self())) {
        flags |= MSG_DONTWAIT;
    }

    if (send(ctx->dispatch_fd, &dispatch, sizeof(dispatch), flags)
            != sizeof(dispatch)) {
        return -errno;
    }
    return 0;
}

/*
 * Initialize an I/O event listener structure and associate it with an event
 * context.
 */
void event_io_init(struct event_context *ctx, struct event_io *io,
        void (*handler)(struct event_io *, uint32_t, void *), void *arg)
{
    EVENT_ASSERT(io != NULL);
    EVENT_ASSERT(handler != NULL);

    io->ctx = ctx;
    io->fd = -1;
    io->event_mask = 0;
    io->handler = handler;
    io->handler_arg = arg;
}

/*
 * Register to get event callbacks for an I/O file descriptor.  This may be
 * called with an already-registered file descriptor to modify the events to
 * listen for.
 * Returns 0 on success, or -errno on failure.
 */
int event_io_register(struct event_io *io, int fd, uint32_t event_mask)
{
    struct epoll_event e = {
        .events = event_mask_to_epoll_events(event_mask),
        .data.ptr = io
    };

    EVENT_ASSERT(io != NULL);
    EVENT_ASSERT(io->ctx != NULL);

    if (fd < 0) {
        /* Invalid file descriptor */
        return -EBADF;
    }
    if (io->fd != -1 && io->fd != fd) {
        /* Listener is already registered to a different FD */
        return -EEXIST;
    }
    if (event_mask == io->event_mask) {
        /* No change */
        return 0;
    }

    if (io->event_mask) {
        /* Modify/remove an existing listener */
        if (event_mask) {
            if (epoll_ctl(io->ctx->epoll_fd, EPOLL_CTL_MOD, io->fd, &e) < 0) {
                return -errno;
            }
        } else {
            epoll_ctl(io->ctx->epoll_fd, EPOLL_CTL_DEL, io->fd, &e);
        }
    } else {
        /* Add a new listener */
        if (epoll_ctl(io->ctx->epoll_fd, EPOLL_CTL_ADD, fd, &e) < 0) {
            return -errno;
        }
        io->fd = fd;
        LIST_INSERT_HEAD(&io->ctx->io_list, io, entry);
    }
    event_io_set_mask(io, event_mask);
    return 0;
}

/*
 * Unregister an I/O event listener.
 * Returns 0 on success, or -errno on failure.
 */
int event_io_unregister(struct event_io *io)
{
    EVENT_ASSERT(io != NULL);

    if (io->fd == -1) {
        /* Not registered */
        return -EBADF;
    }
    LIST_REMOVE(io, entry);

    if (io->event_mask) {
        /*
         * Note: event arg is ignored for EPOLL_CTL_DEL, but Linux
         * kernel < 2.6.9 required a non-NULL argument.  Passing in 1 for
         * portability.
         */
        epoll_ctl(io->ctx->epoll_fd, EPOLL_CTL_DEL, io->fd, (void *)1);
        event_io_set_mask(io, 0);
    }
    io->fd = -1;
    return 0;
}

/*
 * Unregister an I/O event listener and close the associated file descriptor.
 * Returns 0 on success, or -errno on failure.
 */
int event_io_close(struct event_io *io)
{
    int fd;
    int r;

    EVENT_ASSERT(io != NULL);

    fd = io->fd;
    r = event_io_unregister(io);
    if (r < 0) {
        return r;
    }
    close(fd);
    return 0;
}

/*
 * Return the event listener's file descriptor.
 */
int event_io_fd(const struct event_io *io)
{
    return io ? io->fd : -1;
}

/*
 * Search for a registered event listener by file descriptor.
 */
struct event_io *event_io_find_by_fd(const struct event_context *ctx,
        int fd)
{
    struct event_io *listener;

    EVENT_ASSERT(ctx != NULL);

    LIST_FOREACH(listener, &ctx->io_list, entry) {
        if (listener->fd == fd) {
            return listener;
        }
    }
    return NULL;
}

/*
 * Initialize a timer structure and associate it with an event context.  Set
 * the timeout handler and an optional user-specified handler argument.
 */
void event_timer_init(struct event_context *ctx, struct event_timer *t,
        void (*handler)(struct event_timer *, void *), void *arg)
{
    EVENT_ASSERT(ctx != NULL);
    EVENT_ASSERT(t != NULL);
    EVENT_ASSERT(handler != NULL);

    t->ctx = ctx;
    t->time_ms = 0;
    t->repeat_ms = 0;
    t->handler = handler;
    t->handler_arg = arg;
}

/*
 * Set a timer with the specified interval (in milliseconds).  If periodic is
 * true, the timer will repeat indefinitely.  Otherwise, it will run once.
 *
 * For periodic timers, long handler execution times will not skew the timeout
 * period, although entire intervals may be dropped, if they occur in the past.
 */
void event_timer_set(struct event_timer *t, uint64_t interval_ms,
        bool periodic)
{
    EVENT_ASSERT(t != NULL);
    EVENT_ASSERT(t->ctx != NULL);
    EVENT_ASSERT(!periodic || interval_ms > 0);

    event_timer_cancel(t);
    event_timer_insert(t, event_monotonic_ms() + interval_ms);
    if (periodic) {
        t->repeat_ms = interval_ms;
    }
}

/*
 * Set a timer that fires at the specified absolute time (in milliseconds since
 * boot on the monotonic clock).  If repeat_ms is non-zero, the timer will
 * repeat indefinitely with the specified interval.  Otherwise, it will run
 * once.
 *
 * Specifying a start time in the past will result in the timer firing
 * immediately.
 *
 * For periodic timers, long handler execution times will not skew the timeout
 * period, although entire intervals may be dropped, if they occur in the past.
 */
void event_timer_set_abs(struct event_timer *t, uint64_t start_ms,
        uint64_t repeat_ms)
{
    uint64_t cur_time_ms;

    EVENT_ASSERT(t != NULL);
    EVENT_ASSERT(t->ctx != NULL);

    /* Start time must be less than one interval in the past */
    cur_time_ms = event_monotonic_ms();
    if (start_ms <= cur_time_ms - repeat_ms) {
        if (repeat_ms) {
            /* Periodic timers align repeat time to start_ms */
            start_ms = cur_time_ms - ((cur_time_ms - start_ms) % repeat_ms);
        } else {
            start_ms = cur_time_ms;
        }
    }

    event_timer_cancel(t);
    event_timer_insert(t, start_ms);
    t->repeat_ms = repeat_ms;
}

/*
 * Stop a timer.
 */
void event_timer_cancel(struct event_timer *t)
{
    EVENT_ASSERT(t != NULL);

    t->repeat_ms = 0;
    if (!t->time_ms) {
        return;
    }
    /* Remove timer from queue */
    t->time_ms = 0;
    LIST_REMOVE(t, entry);
}

/*
 * Return the number of milliseconds before the timer fires, or -1 if it is not
 * set.
 */
int64_t event_timer_delay_ms(const struct event_timer *t)
{
    uint64_t cur_time_ms;

    EVENT_ASSERT(t != NULL);

    if (!t->time_ms) {
        /* Timer not set */
        return -1;
    }
    cur_time_ms = event_monotonic_ms();
    if (cur_time_ms >= t->time_ms) {
        /* Trigger time is now */
        return 0;
    }
    return t->time_ms - cur_time_ms;
}

/*
 * Return true if a timeout is scheduled.
 */
bool event_timer_active(const struct event_timer *t)
{
    EVENT_ASSERT(t != NULL);

    return t->time_ms != 0;
}

/*
 * Return the number of milliseconds elapsed since boot.  This is is used
 * internally to calculate timer timeout times.
 */
uint64_t event_monotonic_ms(void)
{
    struct timespec now;
    int r;

    do {
        r = clock_gettime(CLOCK_MONOTONIC, &now);
    } while (r < 0 && errno == EINTR);
    EVENT_ASSERT(r == 0);

    return ((uint64_t)now.tv_sec) * 1000 + (uint64_t)(now.tv_nsec / 1000000);
}

#if defined(EVENT_LIBSYSTEMD)
/*
 * sd-event callback to process pending epoll events.
 */
static int event_sdevent_poll_callback(sd_event_source *s, int fd,
        uint32_t revents, void *userdata)
{
    struct event_context *ctx = (struct event_context *)userdata;

    return event_poll(ctx, 0);
}

/*
 * sd-event callback to schedule the next timer callback.
 */
static int event_sdevent_prepare_callback(sd_event_source *s, void *userdata)
{
    struct event_context *ctx = (struct event_context *)userdata;
    struct event_timer *t;
    uint64_t usec, pending_usec;

    t = LIST_FIRST(&ctx->timer_list);
    if (t) {
        /* Schedule timer callback for soonest timer timeout */
        usec = t->time_ms * 1000;
        sd_event_source_get_time(ctx->sd_timer, &pending_usec);
        if (usec != pending_usec) {
            /* Only set time if changed (optimization) */
            sd_event_source_set_time(ctx->sd_timer, t->time_ms * 1000);
        }
        sd_event_source_set_enabled(ctx->sd_timer, SD_EVENT_ONESHOT);
    } else {
        /* No pending timers */
        sd_event_source_set_enabled(ctx->sd_timer, SD_EVENT_OFF);
    }
    return 0;
}

/*
 * sd-event callback to run the expired timers.
 */
static int event_sdevent_timer_callback(sd_event_source *s, uint64_t usec,
        void *userdata)
{
    struct event_context *ctx = (struct event_context *)userdata;

    event_timer_poll(ctx);
    return 0;
}

/*
 * Attach an event context to an existing sd-event loop.  This allows the
 * event interface to be integrated with an application already using the
 * sd-event library.
 * Returns 0 on success, or -errno on failure.
 */
int event_attach_sdevent(struct event_context *ctx, struct sd_event *e)
{
    int r;

    EVENT_ASSERT(ctx != NULL);
    EVENT_ASSERT(e != NULL);

    if (ctx->sd_event) {
        /* Already attached */
        return -EBUSY;
    }
    ctx->sd_event = sd_event_ref(e);

    /* Listen for epoll events */
    r = sd_event_add_io(ctx->sd_event, &ctx->sd_epoll, ctx->epoll_fd,
            EPOLLIN, event_sdevent_poll_callback, ctx);
    if (r < 0) {
        goto error;
    }
    /* Register callback to update sd-event timer before waiting for events */
    r = sd_event_source_set_prepare(ctx->sd_epoll,
            event_sdevent_prepare_callback);
    if (r < 0) {
        goto error;
    }
    /* Create timer to handle timer expiration */
    r = sd_event_add_time(ctx->sd_event, &ctx->sd_timer, CLOCK_MONOTONIC,
            0, 1000, event_sdevent_timer_callback, ctx);
    if (r < 0) {
        goto error;
    }
    sd_event_source_set_enabled(ctx->sd_timer, SD_EVENT_OFF);

    return 0;

error:
    event_detach_sdevent(ctx);
    return r;
}

/*
 * Detach an event context from an sd-event loop.
 */
void event_detach_sdevent(struct event_context *ctx)
{
    EVENT_ASSERT(ctx != NULL);

    ctx->sd_epoll = sd_event_source_unref(ctx->sd_epoll);
    ctx->sd_timer = sd_event_source_unref(ctx->sd_timer);
    ctx->sd_event = sd_event_unref(ctx->sd_event);
}

#endif /* EVENT_LIBSYSTEMD */

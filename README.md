# event
Tiny epoll-based event loop library for event-driven Linux applications.

### Features
* I/O listeners for handling events on sockets and friends
* Timers with millisecond resolution
* Dispatch and exit handling that is thread- and signal-safe and lock-free
* Delightfully simple and clean interface
* Integration with systemd sd-event loop

### Future Work
* Add macros to selectively disable sub-features
* Add file and directory listener support built on inotify
* Add built-in signal handling support (if signalfd can be be setup in a bullet-proof way)
* Add alternate implementation for event.h that provides a callback interface to allow libraries using this event library to integrate with applications built on a different event handling framework (such as libevent, glib, or sd-event).

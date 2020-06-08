# event
Tiny epoll-based event loop library for event-driven Linux applications.

### Features
* I/O listeners for handling events on sockets and friends
* Timers with millisecond resolution
* Dispatch and exit handling that is thread- and signal-safe and lock-free
* Delightfully simple and clean interface
* Integration with systemd sd-event loop

### Future Work
* Add file and directory listener support built on inotify
* Add built-in signal handling support (if signalfd can be be setup in a bullet-proof way)
* Enhance event_dispatch() to allow high throughput capability (currently limited by socket buffer size).

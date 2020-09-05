mq(4) — message queue
====

`Mq` serves a file tree representing buffered two-way data streams
for multiple readers and writers with selectable reading and
writing semantics.

Structure
----

Streams may be created and organized within an arbitrary file tree
structure, providing an obvious way of namespacing and grouping.

A directory denotes a group of streams.  Grouped streams share the same
configuration and may be related by time-based ordering of data
written to them.  The ordering information is provided by a meta-stream
called 'order' that readers, such as `mq-cat(1)`, may use to retrieve data
in a reliable order.

Stream semantics
----

Many aspects of stream behaviour may be chosen at group creation
time to suit different kinds of applications.

* Reading
    * message mode; preserve message boundaries (default)
    * stream mode; coalesce messages (not implemented)
    * (?) static mode; file-like, seekable, eof (not implemented)
* Writing
    * non-blocking
    * blocking; wait one reader, wait all readers (not implemented)
* History replay
    * no replay (default)
    * most recent (not implemented)
    * entire history
* Queue persistence
    * in-memory (default)
    * (?) on disk (not implemented)

Caveats
----

Boundary preservation in message mode only works if none of the
writes exceed the size of the shortest reader. A short reader will receive
the message in two or more parts. There is no way for such a reader
to know that it received split data. This means that readers and writers
must somehow agree on the maximum size of messages that will
be sent on the stream. Without specific agreement the writes should
not exceed 8192 bytes -- the `cat(1)` program buffer size.

Note: current implementation does not split the read response, it just
advances to the next message. This will be fixed.

Access control
----

Access control is provided through filesystem permissions.  By default
only the owner may create and remove groups and streams, as well as
change their configuration.
Authentication, if needed, may be done externally, for example by
wrapping the `mq(4)` channel with `tlssrv(8)`.

Note: current implementation does not check group membership.
This will be fixed.

Examples
----

Mount the `mq(4)` file server and use it to create a detached `rc(1)`
shell that can be attached to from multiple places.

	mq -s detach
	mount -c /srv/detach /n/detach
	mkdir /n/detach/rc
	cd /n/detach/rc
	echo replay on >ctl
	touch fd0 fd1 fd2
	rc -i <fd0 >>fd1 >>[2]fd2 &

Attach to the shell:

	cat fd1 & cat fd2 & cat >>fd0

The included program `attach(1)` provides a more polished interface for
detaching programs, it also makes use of the data ordering information
to faithfully reproduce session history.

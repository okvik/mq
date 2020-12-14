mq(4) — message queue
====

Mq serves a 9p file tree representing groups of buffered two-way data
streams for multiple readers and writers accessible through the
standard read(2) and write(2) file I/O interface.

Overview
----

Streams may be organized within an arbitrary file tree structure, which
provides a means of namespacing and grouping.

	<group>
		<group>*
		<stream>*
		order
		ctl

A directory denotes a group of streams.  Any number of streams and
sub-groups may be created within a group.  Grouped streams share
configuration and an order file.

The read-only meta-stream called order provides ordering information for
data written to streams within a group.  Special readers such as
`mq-cat(1)` can tap into this stream to retrieve data coming from
multiple streams in the same order it was written.

See the [`mq(4)`][mq] manual page for a complete
description of supported data modes, queue replay options, usage reference,
and other details.

Examples
----

Mount the `mq(4)` file server and use it to persist an `rc(1)` shell session.

	mq -s detach
	mount -c /srv/detach /n/detach
	mkdir /n/detach/rc
	cd /n/detach/rc
	echo replay all >ctl
	touch fd0 fd1 fd2
	rc -i <fd0 >>fd1 >>[2]fd2 &

Attach to the shell:

	cat fd1 & cat fd2 & cat >>fd0

The program [`pin(1)`][pin] provides a polished interface for persisting
program sessions.  It also makes use of the data ordering feature for
faithful reproduction of session history.

The program `mq-cat(1)` is an example of an ordered multi-stream reader.

[mq]: http://a-b.xyz/95/34b5
[pin]: http://a-b.xyz/9b/2151

TODO
====

* README
* Manual pages
* Implement Twstat
* Configuration management
	* Options: group permission checking, config freeze
* History management
	* manual truncate
	* hard limit
* Partial reads (count less than corresponding write)
* Oversized writes (count greater than smallest reader iounit)
* Byte-stream read mode
	* Seek
	* Coalesced history replay
* Concurrent readers on the same fid
	* Options: all get the same data (current), each gets unique data
* Re-design attach(1)
	* nice interface
	* note handling
	* ignore stdin replay in current shell
	* (?) meta-commands a là hubshell(1)

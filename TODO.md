TODO
====

* Manual pages
    * mq-cat(1)
* Access control
    * implement Twstat
    * full group permission checking
* Configuration
	* report configuration and status on 'ctl' read
* Queue
	* automatic free in no-replay mode
	* manual clear and limits in replay mode
		* drop very slow readers
* Read
    * coalesced mode
    * concurrent fid readers
      * same data, unique data
    * handling partial reads
* pin(1)
    * note handling
    * (?) erase stdin replay in current shell
    * (?) meta-commands a là hubshell(1)

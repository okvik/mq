TODO
====

* Manual pages
    * mq(4)
    * mq-cat(1)
* Access control
    * implement Twstat
    * full group permission checking
* Configuration management
    * group permission checking
    * configuration reporting on 'ctl'
* History controls
    * manual truncate
    * hard limit
* Read
    * message mode
      * partial read splitting
    * stream mode
    * static mode; seek
    * concurrent fid readers
      * same data, unique data
* Write
    * non-blocking
    * blocking; wait any, wait all
* pin(1)
    * note handling
    * ignore stdin replay in current shell
    * (?) meta-commands a là hubshell(1)

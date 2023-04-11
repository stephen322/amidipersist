# amidipersist
Persist Alsa midi connections.
Watches Alsa system midi events through System:Announce, and will maintain any pre-configured connections.

## Usage
    Usage: ./amidipersist [-f <filename>]	 (default: './amidipersist.connections')

	    File format:
		    File should contain colon-delimited midi names of: src_client:src_port:dst_client:dst_port
		    Colon can be escaped with '\'
		    Empty lines and lines beginning with # are ignored

    Options:
	    --once	Don't persist: run connections once and exit; do not listen for events.
	    

## Requirements
-libasound

## See Also
- [Div's MIDI Utilities](https://github.com/dgslomin/divs-midi-utilities) / alsamidicable
- [naconnect](https://github.com/nedko/naconnect) - an ncurses-based ALSA MIDI sequencer connection manager

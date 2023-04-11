amidipersist: amidipersist.cpp
	g++ -o amidipersist -g3 amidipersist.cpp -lasound

install: amidipersist
	strip amidipersist
	cp -a amidipersist /usr/local/bin/
	chown root:root /usr/local/bin/amidipersist

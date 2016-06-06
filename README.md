# jack_cat

This program records or plays back data from the JACK Audio connection kit.
It's recorded data format is JACK floats.
No conversion to or from any other data format is supported.

'''
jack_cat -c filename | -p filename port(s)
  -c filename    capture to file
  -p filename    play back from file
  -n count       number of ports (do not auto connect)
  -N name        client name to use with jack (default: jack_cat)
  -b size        block size to use
  -B size        ring buffer size
  -t time        run for time seconds
  port1 .. portn names of ports to connect to
'''

If names of ports are given, jack_cat will connect to them.  
If a count of ports is given, jack_cat will create numbered ports that can be
connected to.

This was written to record and play back data from the dttsp SDR.


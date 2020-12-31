# Janus rtp2gstipc plugin

This plugin for the [Janus WebRTC gateway](https://github.com/meetecho/janus-gateway) takes RTP and RTCP packets from a WebRTC connection (Janus session) and forwards/sends them to GstAppSrc ports for further Gstreamer processing.

Four destination UDP addresses/ports are used:

1. Audio RTP
2. Audio RTCP
3. Video RTP
4. Video RTCP


## Compiling and installing

````shell
./bootstrap
./configure --prefix=/usr/local  # or wherever your janus install lives
make
sudo make install  # installs into {prefix}/lib/janus/plugins
````

## Acknowledgements

Thanks go to the authors of [janus-gateway](https://github.com/meetecho/janus-gateway) and michaelfranzl for "Janus rtpforward plugin" ([janus-rtpforward-plugin](https://github.com/michaelfranzl/janus-rtpforward-plugin)).


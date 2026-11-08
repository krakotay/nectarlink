# Fast phone mirroring

Wi-Fi/LAN and USB use the same ordinary hardware H.264 I/P mode. All-Intra is
not enabled. Defaults target 60 fps, a one-second GOP, no B pictures, realtime
priority and zero requested encoder latency. MediaCodec vendors may interpret
latency hints differently; the output format is logged for device diagnosis.
Bitrate targets are 20 Mbps through 1280 pixels, 60 Mbps through 1920 pixels,
and 100 Mbps above that, bounded by the hardware encoder's advertised range.

Video access units use `mirror.datagram.v1` QUIC DATAGRAM. The reliable stream
carries configuration and its acknowledgement. Fragments are at most 1200 bytes;
access units are limited to 2 MiB. The sender keeps one pending access unit and
one in flight, abandons an over-age unit after 35 ms and requests an IDR after
abandoning a dependent chain. Producer sequence numbers include dropped frames.
It never silently switches to a reliable video stream when DATAGRAM is missing.
Audio, input and configuration retain their own reliable control/stream paths.
Existing reverse desktop capture is outside this phone-to-PC mode.

The receiver holds at most four access units, allows 5 ms for inter-frame
reordering and 35 ms for fragment assembly. Loss or expiry stops dependent
pictures until a complete IDR. Keyframe requests retry every 100 ms. A complete
new IDR supersedes incomplete older frames. An excess transit estimate rejects
frames that aged in the network without requiring synchronized device clocks.
USB uses the same application mode; a TCP-based USB tunnel can still have its
own transport-level head-of-line delay.

Decoding and presentation have separate workers. The completed-picture mailbox
and Qt's view cache retain only the latest picture, while decoding preserves
all accepted references. Linux requires Vulkan 1.3, Vulkan Video H.264 decoding,
timeline semaphores, synchronization2 and two graphics/compute queues on the
selected GPU. One shared device feeds FFmpeg and Qt, on separate queues.
NV12 planes are copied inside the GPU to sampled R8/RG8 images, then converted
in a YUV shader. Presentation does not download GPU frames to CPU memory.
An explicit screenshot does perform a readback. This is a GPU-to-GPU copy path,
not literal zero-copy. Surface ownership and GPU completion fences prevent
recycling a decoder frame or sampled image while it is in use.

Unsupported devices and initialization/render/decoder failures produce explicit
errors. There is no Linux mirror substitution to OpenGL, CUDA, software decode,
CPU color conversion or another FFmpeg ABI. The selected Qt SDK and qsb are build
requirements; the Debian package depends on the built-in FFmpeg ABI and Vulkan
loader. The user's device tests must establish actual latency and vendor encoder
behavior; build and protocol tests do not establish a latency improvement.

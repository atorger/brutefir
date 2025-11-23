
# BruteFIR

BruteFIR is a software convolution engine, a program for applying long
FIR filters to multi-channel digital audio, either offline or in
realtime. Its basic operation is specified through a configuration
file, and filters, attenuation and delay can be changed in runtime
through a simple command line interface. The FIR filter algorithm used
is an optimized and partitioned frequency domain algorithm, thus
throughput is extremely high, and I/O-delay can be kept fairly low.

This is legacy project but is still maintained as it has become a
popular reference no-nonsense convolution engine.

This github repository is "raw" available just for reference and to
make easier to handle bug reports. For users it's better to use the
actual releases (made with "make distrib") which is available for
download on the project's homepage:

https://www.torger.se/anders/brutefir.html
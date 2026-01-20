
# BruteFIR

BruteFIR is a software convolution engine, a program for applying long
FIR filters to multi-channel digital audio, either offline or in
realtime. Its basic operation is specified through a configuration
file, and filters, attenuation and delay can be changed in runtime
through a simple command line interface. The FIR filter algorithm used
is an optimized partitioned frequency domain algorithm, thus throughput
is extremely high, and I/O-delay can be kept fairly low.

This is a legacy project, but is still maintained as it has become a
popular reference no-nonsense convolution engine.

This github repository is "raw" available just for reference and to
make it easier to handle bug reports. The github workflow for releases
and packages is not used.

For regular users it's recommended to use the actual releases (made
with "make distrib") which is available for download on the project's
homepage:

https://www.torger.se/anders/brutefir.html
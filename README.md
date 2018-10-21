## Synopsis

The Network Block Device is a Linux-originated lightweight block access
protocol that allows one to export a block device to a client.

This client is designed to run on top of FreeBSD's GEOM Gate device driver,
keeping the network client in a userland daemon rather than in a kernel
module.

Casper is used to sandbox the DNS lookup of the server host as well as for
logging messages to syslog.  Capsicum is used to limit the capabilities of the
client once connected.

The client immediately daemonizes unless passed the `-f` flag, in which
case it remains running in the foreground.

## Caveats

* TLS and other extensions are not currently supported.
* Only the default (unnamed) export is used.
* Manual control (listing, naming, numbering, removal) of the device nodes
  is not yet provided by this tool.  Use ggatec instead.

## Usage Example

Connect to an NBD server and print the name of the new device on stdout:

```
nbd-client 192.168.1.101
```

Connect specifying a hostname and port number:

```
nbd-client nbd.storage.internal-domain.net 10809
```

You can then use this device as a regular disk device.  For example, create
a ZFS pool named `foo` backed by the NBD storage attached to `ggate0`.

```
zpool create foo ggate0
```

Use the `ggatec` utility to list all attached GEOM Gate devices.  These may
not all be NBD devices:

```
ggatec list
```

Use the `ggatec` utility to force removal of the GEOM Gate device unit `0`,
which corresponds to the device named `ggate0`:

```
ggatec destroy -f -u 0
```

## Compiling

```
make
make install # (optional)
```

Note: This project expects to be compiled with BSD make, not GNU make.

## To Do

* specify named export
* list exports (work started)
* connect to multiple/all exports on a server (spawn a thread per export?)
* configuration file
* rc scripts
* option to drop to a less privileged user?
* Kyua tests?

## Bugs

Please report them along with steps to reproduce.

## Motivation

Scaleway offers high-density dedicated servers using NBD to attach storage
to each machine.  FreeBSD lacks an NBD client, so Scaleway does not support
FreeBSD.  This project aims to implement an NBD client so that Scaleway can
offer FreeBSD on their servers.

## License

BSD 2-Clause License

Copyright (c) 2016, Ryan Moeller
All rights reserved.

Redistribution and use in source and binary forms, with or without
modification, are permitted provided that the following conditions are met:

* Redistributions of source code must retain the above copyright notice, this
  list of conditions and the following disclaimer.

* Redistributions in binary form must reproduce the above copyright notice,
  this list of conditions and the following disclaimer in the documentation
    and/or other materials provided with the distribution.

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE
FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY,
OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.

# galmon
galileo/GPS/GLONASS/BeiDou open source monitoring. GPL3 licensed. 
(C) AHU Holding BV - bert@hubertnet.nl - https://ds9a.nl/

Live website: https://galmon.eu/

Theoretically multi-vendor, although currently only the U-blox 8 chipset is
supported. Navilock NL-8012U receiver works really well, as does the U-blox evaluation kit for the 8MT.

Highlights:
 * Processes raw frames/strings/words from GPS, GLONASS, BeiDou and Galileo
 * Calculate ephemeris positions
 * Record discontinuities between subsequent ephemerides
 * Compare doppler shift as reported by receiver with that expected from ephemeris
 * Track atomic clock & report jumps
 * Compare orbit to TLE, match up to best matching satellite
 * Tear out every bit that tells us how well an SV is doing
 * Full almanac processing to see what _should_ be transmitting
 * Distributed receivers, combined into a single source of all messages

Goals:

1) Support multiple wildly distributed receivers
2) Combine these into a forensic archive of all Galileo/GPS NAV messages
3) Make this archive available, offline and as a stream
4) Consume this stream and turn it into an attractive live website
   (https://galmon.eu/). As part of this, perform higher-level calculations
   to determine ephemeris discontinuities, live gst/gps/galileo time
   offsets, atomic clock jumps etc.
5) Populate an InfluxDB timeseries database with raw measurements and higher
   order calculations

Works on Linux (including Raspbian on Pi Zero W), OSX and OpenBSD.



Build locally
-------------

To get started, make sure you have a C++17 compiler, git, protobuf-compiler.
Then run 'make ubxtool navdump' to build the receiver-only tools.

To also run the webserver locally, intall libh2o-dev and run 'make'.

To build everything, try:

```
apt-get install protobuf-compiler libh2o-dev libcurl4-openssl-dev libssl-dev libprotobuf-dev libh2o-evloop-dev libwslay-dev libncurses5-dev
git clone https://github.com/ahupowerdns/galmon.git --recursive
cd galmon
make
```

If this doesn't succeed with an error about h2o, make sure you have this
library installed. If you get an error about 'wslay', do the following, and run make again:

```
echo WSLAY=-lwslay > Makefile.local
```

Build in Docker
---------------

To build it in Docker:

```
git clone https://github.com/ahupowerdns/galmon.git --recursive
docker build -t galmon --build-arg MAKE_FLAGS=-j2 .
```

To run a container with a shell in there:

```
docker run -it --rm galmon
```


Running
-------

Once compiled, run for example `./ubxtool --wait --port /dev/ttyACM0
--station 1 --stdout | ./navparse 10000 html null`

Next up, browse to http://[::1]:10000 (or try http://localhost:10000/ and
you should be in business. ubxtool changes (non-permanently) the
configuration of your u-blox receiver so it emits the required frames for
GPS and Galileo. If you have a u-blox timing receiver it will also enable
the doppler frames.

To see what is going on, try:

```
./ubxtool --wait --port /dev/ttyACM0 --station 1 --stdout | ./navdump
```

To distribute data to a remote `navrecv`, use:

```
./ubxtool --wait --port /dev/ttyACM0 --station 255 --dest 127.0.0.1
```

This will send protobuf to 127.0.0.1:29603. You can add as many destinations
as you want, they will buffer and automatically reconnect. To also send data
to stdout, add `--stdout`.

Tooling:

 * ubxtool: can configure a u-blox 8 chipset, parses its output & will
   convert it into a protbuf stream of GNSS NAV frames + metadata
   Adds 64-bit timestamps plus origin information to each message
 * xtool: if you have another chipset, build something that extracts NAV
   frames & metadata. Not done yet.
 * navrecv: receives GNSS NAV frames and stores them on disk, split out per
   sender. 
 * navnexus: tails the files stored by navrecv, makes them available over
   TCP
 * navparse: consumes these ordered nav updates for a nice website
   and puts "ready to graph" data in influxdb - this is the first
   step that breaks "store everything in native format". Also does
   computations on ephemerides. 
 * grafana dashboard: makes pretty graphs

Linux Systemd
-------------

This is very much a first stab. Do the following at root.
```
mkdir /run/ubxtool
mkdir /usr/local/ubxtool
cp ubxtool.sh /usr/local/ubxtool/
cp ubxtool /usr/local/ubxtool/
cp ubxtool.service /etc/systemd/system/ubxtool.service
touch /usr/local/ubxtool/destination
touch /usr/local/ubxtool/station
```
Then edit /usr/local/ubxtool/destination with an IP address collected from Bert.
Then edit /usr/local/ubxtool/station with a station number collected from Bert.

The start up the service.
```
sudo systemctl enable ubxtool
sudo systemctl start ubxtool
```
This will be cleaned up and better packaged sometime soon.

Distributed setup
-----------------
Run `navrecv :: ./storage` to receive frames on port 29603 of ::, aka all your IPv6 addresses (and IPv4 too on Linux).
This allows anyone to send you frames, so be aware.

Next up, run `navnexus ./storage ::`, which will serve your recorded data from port 29601. It will merge messages
coming in from all sources and serve them in time order.

Finally, you can do `nv 127.0.0.1 29601 | ./navdump`, which will give you all messages over the past 24 hours, and stream you more.
This also works for `navparse` for the pretty website and influx storage, `nc 127.0.0.1 29601 | ./navparse 127.0.0.0:10000 html galileo`,
if you have an influxdb running on localhost with a galileo database in there.

Internals
---------
The transport format consists of repeats of:

1) Four byte magic value
2) Two-byte frame length
3) A protobuf frame

The magic value is there to help us resync from partially written data.

The whole goal is that we can continue to rebuild the database by 
rerunning 'navstore' and 'navinflux'.

Documents
---------

 * [BeiDou](http://m.beidou.gov.cn/xt/gfxz/201902/P020190227593621142475.pdf)
 * [Galileo](https://www.gsc-europa.eu/sites/default/files/sites/all/files/Galileo-OS-SIS-ICD.pdf)
 * [GLONASS](https://www.unavco.org/help/glossary/docs/ICD_GLONASS_4.0_(1998)_en.pdf),
    old 1998 version, but unlike newer versions, this one is not full of
    mistakes. [New version](http://gauss.gge.unb.ca/GLONASS.ICD.pdf) is more complete but is worryingly messy.
 * [GLONASS CDMA](http://russianspacesystems.ru/wp-content/uploads/2016/08/ICD-GLONASS-CDMA-General.-Edition-1.0-2016.pdf)
   not actually relevant for the CDMA aspects, but has appendices on more
   precise orbit determinations.
 * [GPS](https://www.gps.gov/technical/icwg/IS-GPS-200K.pdf)
 * [U-blox 8 interface specification](https://www.u-blox.com/sites/default/files/products/documents/u-blox8-M8_ReceiverDescrProtSpec_%28UBX-13003221%29_Public.pdf)
 * [U-blox 9 interface specification](https://www.u-blox.com/sites/default/files/u-blox_ZED-F9P_InterfaceDescription_%28UBX-18010854%29.pdf)


Big TODO
--------

 * Dual goals: completeness, liveness, not the same
   For forensics, great if the packet is there
   For display, not that bad if we missed a message
 * In general, consider refeed strategy
     Raw serial
     Protobuf
     Influxdb
     ".csv files"
 * Delivery needs to be bit more stateful (queue)
   
 * Semantics definition for output of Navnexus
   "we'll never surprise you with old data"

ubxtool
-------
 * Will also spool raw serial data to disk (in a filename that includes the
   start date)
 * Can also read from disk
 * Careful to add the right timestamps


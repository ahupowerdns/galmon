# galmon
galileo/GPS/GLONASS/BeiDou open source monitoring. GPL3 licensed. 
(C) AHU Holding BV - bert@hubertnet.nl - https://berthub.eu/

Live website: https://galmon.eu/

Multi-vendor, with support for U-blox 8 and 9 chipsets and many Septentrio
devices.  Navilock NL-8012U receiver works really well, as does the U-blox
evaluation kit for the 8MT.  In addition, many stations have reported
success with this very cheap [AliExpress sourced
device](https://www.aliexpress.com/item/32816656706.html).  

For ublox, there is good support for the F9P, several of us use the
[ArdusimpleRTK2B](https://www.ardusimple.com/simplertk2b/) board. It adds
the Galileo E5b band.

Septentrio devices support even more bands.

An annotated presentation about our project aimed at GNSS professionals can
be found [here](https://berthub.eu/galileo/The%20galmon.eu%20project.pdf). 

> NOTE: One of our programs is called 'ubxtool'. Sadly, we did not do our
> research, and there is another '[ubxtool](https://gpsd.io/ubxtool.html)' already, part of
> [gpsd](https://gpsd.io). You might have ended up on our page by mistake.
> Sorry!

To deliver data to the project, please read
[The Galmon GNSS Monitoring Project](https://berthub.eu/articles/posts/galmon-project/)
and consult the rules outlined in [the operator
guidelines](https://github.com/ahupowerdns/galmon/blob/master/Operator.md).

Highlights
----------

 * Support for Septentrio and U-blox.
 * Processes raw frames/strings/words from GPS, GLONASS, BeiDou and Galileo
 * All-band support (E1, E5a, E5b, B1I, B2I, Glonass L1, Glonass L2, GPS L1C/A)
   so far.
 * Calculate ephemeris positions
 * Comparison of ephemerides to independent SP3 data to determine SISE
   * Globally, locally, worst user location
 * Record discontinuities between subsequent ephemerides (in time and space)
 * Compare doppler shift as reported by receiver with that expected from ephemeris
 * Track atomic clock & report jumps
 * Coverage maps (number of satellites >5, >10, >20 elevation)
 * HDOP/VDOP/PDOP maps
 * Compare orbit to TLE, match up to best matching satellite
 * Tear out every bit that tells us how well an SV is doing
 * Full almanac processing to see what _should_ be transmitting
 * Distributed receivers, combined into a single source of all messages
 * Ready to detect/report spoofing/jamming

Data is made available as JSON, as a user-friendly website and as a
time-series database. This time-series database is easily mated to the
industry standard Matplotlib/Pandas/Jupyter combination (details 
[here](https://github.com/ahupowerdns/galmon/blob/master/influxdb.md).

There is also tooling to extract raw frames/strings/words from specific
timeframes.

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

Works on Linux (including Raspbian Buster on Pi Zero W), OSX and OpenBSD.

Build locally
-------------

To get started, make sure you have a C++17 compiler (like g++ 8 or higher),
git, protobuf-compiler.  Then run 'make ubxtool navdump' to build the
receiver-only tools.

To build everything, including the webserver, try:

```
apt-get install protobuf-compiler libh2o-dev libcurl4-openssl-dev libssl-dev libprotobuf-dev \
libh2o-evloop-dev libwslay-dev libncurses5-dev libeigen3-dev libzstd-dev
git clone https://github.com/ahupowerdns/galmon.git --recursive
cd galmon
make
```

If this doesn't succeed with an error about h2o, make sure you have this
library installed. If you get an error about 'wslay', do the following, and run make again:

```
echo WSLAY=-lwslay > Makefile.local
```

Running in Docker
-----------------

We publish official Docker images for galmon on
[docker hub](https://hub.docker.com/r/berthubert/galmon) for multiple architectures.

To run a container with a shell in there (this will also expose a port so you
can view the UI too and assumes a ublox GPS device too -
you may need to tweak as necessary):

```
docker run -it --rm --device=/dev/ttyACM0 -p 10000:10000 berthubert/galmon
```

Running a daemonized docker container reporting data to a remote server
might look like:

```
docker run -d --restart=always --device=/dev/ttyACM0 --name=galmon berthubert/galmon ubxtool --wait --port /dev/ttyACM0 --gps --galileo --glonass --destination [server] --station [station-id] --owner [owner]
```

To make your docker container update automatically you could use a tool such as
[watchtower](https://containrrr.github.io/watchtower/).

Build `ubxtool` in Docker
-------------------------

To build a minimal `ubxtool` docker image:

```
git clone https://github.com/ahupowerdns/galmon.git --recursive
./build-ubxtool
```

It may also cross-compile a binary and an image for a Raspberry Pi:

```
git clone https://github.com/ahupowerdns/galmon.git --recursive
docker run -it --rm --privileged multiarch/qemu-user-static:register --reset
./build-ubxtool armv6l
```

Running
-------
On u-blox: 
Once compiled, run for example `./ubxtool --wait --port /dev/ttyACM0
--station 1 --stdout --galileo | ./navparse --bind [::1]:10000`

For Septentrio, try: `nc 192.168.1.1 29000 | ./septool --station x --stdout |
./navparse --bind [::1]:10000`, assuming your Septentrio can be reached on
192.168.1.1.1 and you have defined an SBF stream on port 29000. For more
details, please see below.

Next up, browse to http://[::1]:10000 (or try http://localhost:10000/ and
you should be in business. ubxtool changes (non-permanently) the
configuration of your u-blox receiver so it emits the required frames for
GPS and Galileo. If you have a u-blox timing receiver it will also enable
the doppler frames.

By default the ublox receiver module will be configured to use the USB port,
if you want to use a different interface port on the ublox module then add
the `--ubxport <id>` option using one of the following numeric IDs:

   0 : DDC (aka. I2C)  
   1 : UART[1]  
   2 : UART2  
   3 : USB (default)  
   4 : SPI  

To see what is going on, try:

```
./ubxtool --wait --port /dev/ttyACM0 --station 1 --stdout --galileo | ./navdump
```

To distribute data to a remote `navrecv`, use:

```
./ubxtool --wait --port /dev/ttyACM0 --galileo --station 255 --destination 127.0.0.1
```

This will send protobuf to 127.0.0.1:29603. You can add as many destinations
as you want, they will buffer and automatically reconnect. To also send data
to stdout, add `--stdout`.

Tooling:

 * ubxtool: can configure a u-blox 8 chipset, parses its output & will
   convert it into a protbuf stream of GNSS NAV frames + metadata
   Adds 64-bit timestamps plus origin information to each message
 * septool: ingests the Septentrio binary format (SBF) and converts it to our
   protobuf format. Supports same protocol as ubxtool.
 * rtcmtool: ingest ntripclient output, decodes RTCM messages and converts
   them to our protobuf format
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
First make sure 'ubxtool' has been compiled (run: make ubxtool). Then, as
root:
```
mkdir /usr/local/ubxtool
cp ubxtool ubxtool.sh /usr/local/ubxtool/
cp ubxtool.service /etc/systemd/system/
```

Then please reach out as indicated in [Operator.md](Operator.md) to obtain your
station ID and the receiver hostname and run:

```
echo RECEIVER-NAME > /usr/local/ubxtool/destination
echo STATION-NUMBER > /usr/local/ubxtool/station
```

Then start up the service (as root):
```
systemctl enable ubxtool
systemctl start ubxtool
```

To check if it is all working, do 'service ubxtool status'.

> NOTE!  If you don't use one of the AliExpress or Navilock devices, it may
> be that your U-blox is not connected to the USB-port of the U-blox chip
> but to the UART1 or UART2 port.  If so, you'll need to edit the script so
> it finds your USB-to-serial adapter.  At the very least you'll have to
> update the DEVICE line.  You'll likely also have to add --ubxport 1 at the
> end, and likely also the baudrate (-b) and/or --rtscts=0.

To change the default constellations, create a file called
/usr/local/ubxtool/constellations and set your favorites. To set all four
constellations (which only F9-receivers support), do as root:

```
echo --gps --glonass --beidou --galileo > /usr/local/ubxtool/constellations
```

And then 'service ubxtool restart'.

Distributed setup
-----------------
Run `navrecv -b :: --storage ./storage` to receive frames on port 29603 of
::, aka all your IPv6 addresses (and IPv4 too on Linux).  This allows anyone
to send you frames, so be aware.

Next up, run `navnexus --storage ./storage -b ::`, which will serve your
recorded data from port 29601.  It will merge messages coming in from all
sources and serve them in time order.

Finally, you can do `nc 127.0.0.1 29601 | ./navdump`, which will give you all messages over the past 24 hours, and stream you more.
This also works for `navparse` for the pretty website and influx storage, `nc 127.0.0.1 29601 | ./navparse --influxdb=galileo`,
if you have an influxdb running on localhost with a galileo database in there.
The default URL is http://127.0.0.1:29599/ 

Septentrio specifics
--------------------
Unlike `ubxtool`, our `septool` does not (re)configure your Septentrio
device. Instead, the tool expects Septentrio Binary Format (SBF) on input,
and that this stream includes at least the following messages:

 * MeasEpoch
 * PVTCartesian

We currently parse and understand:
 * GALRawFNAV
 * GALRawINAV

Support will be added soon for:
 * GPSRawCA
 * GPSRawL2C
 * GPSRawL5
 * GLORawCA
 * BDSRaw
 * BDSRawB1C
 * BDSRawB2a

A typical invocation of `septool` looks like this:

```
nc 192.168.1.1 29000 | ./septool --station x --destination galmon-eu-server.example.com
```

Or to test, try:

```
nc 192.168.1.1 29000 | ./septool --station x --stdout | ./navdump
```

This is assuming that you can reach your Septentrio on 192.168.1.1 and that
you have defined a TCP server stream on port 29000. 

Septool will also accept input from a serial port or basically anything that
can provide SBF. Please let us know if our tooling can make your life
easier.

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
 * [U-blox 9 integration manual](https://www.u-blox.com/sites/default/files/ZED-F9P_IntegrationManual_%28UBX-18010802%29.pdf)

Data sources
------------
The software can interpret SP3 files, good sources:

 * ESA/ESOC: http://navigation-office.esa.int/products/gnss-products/ - pick the
   relevant GPS week number, and then a series (.sp3 extension):
   * ESU = ultra rapid, 2-8h delay, only GPS and GLONASS
   * ESR = rapid, 2-26h delay, only GPS and GLONASS
   * ESM = finals, 6-13d delay, GPS, GLONASS, Galileo, BeiDou, QZSS
   * File format is esXWWWWD.sp3 - where X is U, R or M, WWWW is the
   (non-wrapping) GPS week number and D is day of week, Sunday is 0.
   * Further description: http://navigation-office.esa.int/GNSS_based_products.html
 * GFZ Potsdam: ftp://ftp.gfz-potsdam.de/GNSS/products/mgnss
   * The GBM series covers GPS, GLONASS, Galileo, BeiDou, QZSS and appears
     to have less of a delay than the ESA ESM series.
   * GBU = ultra rapid, still a few days delay, but much more recent.

To get SP3 GBM from GFZ Potsdam for GPS week number 2111:

```
WN=2111
lftp -c "mget ftp://ftp.gfz-potsdam.de/GNSS/products/mgnss/${WN}/gbm*sp3.Z"
gunzip gbm*sp3.Z
```

To feed data, use: 

```
./sp3feed --sp3src=gbm --influxdb=galileo gbm*sp3
```

This will populate the sp3 tables in the database. A subsequent run of
`reporter`, while setting the `--sp3src` parameter, will provide SP3
deviation statistics, and fill out the `sp3delta` table in there, which
stores deviations from the SP3 provided position, per sp3src.

Further interesting (ephemeris) data is on http://mgex.igs.org/IGS_MGEX_Products.php

RTCM
----
RTCM is the Radio Technical Commission for Maritime Services, and
confusingly, also the name of a protocol. 

This protocol is proprietary, but search for a file called `RTCM3.2.pdf` or
`104-2013-SC104-STD - Vers. 3.2.docx` and you might find a copy. 

This project can parse RTCM 10403.1 messages, and currently processes State
Space Representation (SSR) messages, specifically types 1057/1240
(GPS/Galileo Orbit corrections to broadcast ephemeris) and 1058/1241
(GPS/Galileo Clock corrections to broadcast ephemeris).

RTCM messages need to be converted to protobuf format, and the `rtcmtool` is
provided for this purpose.

RTCM is frequently transmitted over the internet using 'ntrip', a typical
commandline to process RTCM in our project is:
```
$ ntripclient ntrip:CLKA0_DEU1/user:password@navcast.spaceopal.com:2101 | ./rtcmtool --station x --destination y
```

User and password can be obtained from https://spaceopal.com/navcast/ - the
Galileo operating company. 

The IGS also offers excellent streams, but without Galileo. Information is
[here](http://www.igs.org/rts/products). A typical commandline is:

```
$ ntripclient ntrip:IGS01/user:password@products.igs-ip.net:2101 | ./rtcmtool --station x --destination y
```

User and password can be requested through http://www.igs.org/rts/access

An interesting list is here: http://products.igs-ip.net/

There are many other sources of RTCM but currently not many offer the SSR
messages we can use.

Tooling
-------

 * ubxtool: Configure and use a Ublox chip to gather messages, and send as
   protobuf to standard output or a remote server (with buffering).
 * navdump: convert protobuf format data into a textual display of messages
 * navparse: consume protobuf and turn into a webserver with data, plus
   optionally fill an influxdb time-series database for graphing and analysis
   purposes.
 * navrecv: receive protobuf messages over the network and store them on
   disk
 * navnexus: serve protobuf messages from disk over the network
 * navcat: serve protobuf messages from disk directly to stdout
 * reporter: make "the galmon.eu weekly galileo report"
 * rinreport: rinex analysis tooling (not generically useful yet)
 * galmonmon: monitor a navparse instance for changes, tweet them out
 * navdisplay: some eye-candy that converts protobuf into a live display
   (not very good)
 * rtcmtool: accepts RTCM messages on standard input (for example coming
   from ntripclient) and transmits them as protobuf messages, either to
   stdout or to a navrecv server. This is the equivalent of 'ubxtool'
   except for submitting RTCM messages. 

Sample command lines
--------------------
Look at old data:

```
$ ./navcat storage "2020-01-01 00:00" "2020-01-02 00:00" | ./navdump  
```

Global coverage (via volunteers)
--------------------------------

In alphabetical order:

 * Austria (Vienna area)
 * Brazil
 * Holland (Nootdorp, Hilversum, etc)
 * India (New Delhi area)
 * Israel (Jerusalem)
 * Italy (Rome)
 * New Zealand (Auckland area)
 * Rusia (Moscow area)
 * Singapore
 * South Africa (Cape Town area)
 * Spain
 * Tonga 
 * USA
   * Alaska (Anchorage)
   * California (Santa Cruz, Los Angeles area, etc)
   * Massachusetts (Boston area)
 * Uruguay
 
Additional sites are welcome (and encouraged) as the more data receiving sites that exist, then more accurate data and absolute coverage of each constellation can be had.

The galmon project is very grateful to all its volunteering receiving stations.

ubxtool
-------
 * Will also spool raw serial data to disk (in a filename that includes the
   start date)
 * Can also read from disk
 * Careful to add the right timestamps


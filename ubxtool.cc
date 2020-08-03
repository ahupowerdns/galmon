#define _LARGEFILE64_SOURCE
#include <sys/types.h>                                                    
#include <sys/time.h>
#include <map>
#include <sys/stat.h>                                                     
#include <fcntl.h>                                                        
#include <termios.h>                                                      
#include <stdio.h>                                                        
#include <unistd.h>
#include <strings.h>
#include <stdlib.h>
#include <string>
#include <algorithm>
#include <random>
#include <stdint.h>
#include "ubx.hh"
#include "navmon.hh"
#include <iostream>
#include <fstream>
#include <string.h>
#include "fmt/format.h"
#include "fmt/printf.h"
#include "bits.hh"
#include "galileo.hh"
#include <arpa/inet.h>
#include "navmon.pb.h"
#include "gps.hh"
#include "glonass.hh"
#include "beidou.hh"
#include "CLI/CLI.hpp"
#include <thread>
#include <mutex>
#include "comboaddress.hh"
#include "swrappers.hh"
#include "sclasses.hh"
#include "nmmsender.hh"
#include "version.hh"

static char program[]="ubxtool";

bool doDEBUG{false};
bool doLOGFILE{false};
bool doVERSION{false};
bool doSILENT{false};

struct timespec g_gnssutc;
uint16_t g_galwn;

using namespace std;

uint16_t g_srcid{0};
int g_fixtype{-1};
double g_speed{-1};
extern const char* g_gitHash;

static int getBaudrate(int baud)
{
  if(baud==115200)
    return B115200;
  else if(baud==57600)
    return B57600;
  else if(baud==38400)
    return B38400;
  else if(baud==19200)
    return B19200;
  else if(baud==9600)
    return B9600;
  else
    throw std::runtime_error("Unknown baudrate "+to_string(baud));
}

static int getBaudrateFromSymbol(int baud)
{
  if(baud==B115200)
    return 115200;
  else if(baud==B57600)
    return 57600;
  else if(baud==B38400)
    return 38400;
  else if(baud==B19200)
    return 19200;
  else if(baud==B9600)
    return 9600;
  else
    throw std::runtime_error("Unknown baudrate symbol "+to_string(baud));
}

static int g_baudval;

/* inav schedule:
1) Find plausible start time of next cycle
   Current cycle: TOW - (TOW%30)
   Next cycle:    TOW - (TOW%30) + 30

E1:

t   n   w
0   1:  2                   tow % 30 == 1           
2   2:  4                   tow % 30 == 3           
4   3:  6             TOW               5          -> set startTow, startTowFresh
6   4:  7/9                             7           
8   5:  8/10                            9          
10  6:  0          WN/TOW              11            
12  7:  0          WN/TOW              13
14  8:  0          WN/TOW              15
16  9:  0          WN/TOW              17
18  10: 0          WN/TOW              19
20  11: 1                              21          
22  12: 3                              23
24  13: 5          WN/TOW              25
26  14: 0          WN/TOW              27
28  15: 0          WN/TOW              29

E5b-1:
t   n   w
0   1:  1 (2/2)             tow % 30 == 0           
2   2:  3                   tow % 30 == 2           
4   3:  5          WN/TOW               4           -> set startTow, startTowFresh
6   4:  7/9                                       
8   5:  8/10                                       
10  6:  0          WN/TOW                          
12  7:  0          WN/TOW
14  8:  0          WN/TOW
16  9:  0          WN/TOW
18  10: 0          WN/TOW
20  11: 2                                          
22  12: 4
24  13: 6             TOW
26  14: 0          WN/TOW
28  15: 1 (1/2)    WN/TOW

*/

class UBXMessage
{
public:
  struct BadChecksum{};
  explicit UBXMessage(basic_string_view<uint8_t> src)
  {
    d_raw = src;
    if(d_raw.size() < 6)
      throw std::runtime_error("Partial UBX message");

    uint16_t csum = calcUbxChecksum(getClass(), getType(), d_raw.substr(6, d_raw.size()-8));
    if(csum != d_raw.at(d_raw.size()-2) + 256*d_raw.at(d_raw.size()-1))
      throw BadChecksum();
    
  }
  uint8_t getClass() const
  {
    return d_raw.at(2);
  }
  uint8_t getType() const
  {
    return d_raw.at(3);
  }
  std::basic_string<uint8_t> getPayload() const
  {
    return d_raw.substr(6, d_raw.size()-8);
  }
  std::basic_string<uint8_t> d_raw;
};

bool g_fromFile{false};

#if !defined(O_LARGEFILE)
#define O_LARGEFILE	0
#endif

std::pair<UBXMessage, struct timeval> getUBXMessage(int fd, double* timeout)
{
  static int logfile=0;
  if (doLOGFILE) {
    if(!logfile && !g_fromFile) {
    logfile = open("./logfile", O_WRONLY|O_CREAT|O_APPEND|O_LARGEFILE, 0600);
      if(!logfile)
        throw std::runtime_error("Failed to open logfile for writing");
    }
  }
  uint8_t marker[2]={0};
  for(;;) {
    marker[0] = marker[1];
    int res = readn2Timeout(fd, marker+1, 1, timeout);

    if(res < 0) {
      cerr<<"Readn2Timeout failed: "<<strerror(errno)<<endl;
      throw EofException();
    }
    
    //    if (doDEBUG) { cerr<<humanTimeNow()<<" marker now: "<< (int)marker[0]<<" " <<(int)marker[1]<<endl; }
    if(marker[0]==0xb5 && marker[1]==0x62) { // bingo
      struct timeval tv;
      gettimeofday(&tv, 0);
      basic_string<uint8_t> msg;
      msg.append(marker, 2);  // 0,1
      uint8_t b[4];
      readn2Timeout(fd, b, 4, timeout);
      msg.append(b, 4); // class, type, len1, len2

      uint16_t len = b[2] + 256*b[3];
      //      if (doDEBUG) { cerr<<humanTimeNow()<<" Got class "<<(int)msg[2]<<" type "<<(int)msg[3]<<", len = "<<len<<endl; }
      uint8_t buffer[len+2];
      res=readn2Timeout(fd, buffer, len+2, timeout);

      msg.append(buffer, len+2); // checksum
      if (doLOGFILE) {
        if(!g_fromFile)
          writen2(logfile, msg.c_str(), msg.size());      
      }
      return make_pair(UBXMessage(msg), tv);
    }
  }                                                                       
}

UBXMessage waitForUBX(int fd, int seconds, uint8_t ubxClass, uint8_t ubxType)
{
  double timeout = seconds;
  for(;;) {
    auto [msg, tv] = getUBXMessage(fd, &timeout);
    (void) tv;

    if(msg.getClass() == ubxClass && msg.getType() == ubxType) {
      return msg;
    }
    else
      if (doDEBUG) { cerr<<humanTimeNow()<<" Got: "<<(int)msg.getClass() << " " <<(int)msg.getType() <<" while waiting for "<<(int)ubxClass<<" " <<(int)ubxType<<endl; }
  }
  throw std::runtime_error("Did not get response on time");
}

UBXMessage sendAndWaitForUBX(int fd, int seconds, basic_string_view<uint8_t> msg, uint8_t ubxClass, uint8_t ubxType)
{
  for(int n=3; n; --n) {
    writen2(fd, &msg[0], msg.size());
    try {
      return waitForUBX(fd, seconds, ubxClass, ubxType);
    }
    catch(...) {
      if(n==1)
        throw;
      if (!doSILENT) { cerr<<"Retransmit"<<endl; }
    }
  }
  // we actually never get here, but if you remove this line, we get a warning
  // and people can't stop nagging us about the warning..
  return waitForUBX(fd, seconds, ubxClass, ubxType);
}


bool waitForUBXAckNack(int fd, int seconds, int ubxClass, int ubxType)
{
  double timeout = seconds;
  for(;;) {
    auto [msg, tv] = getUBXMessage(fd, &timeout);
    (void)tv;

    if(msg.getClass() != 5 || !(msg.getType() == 0 || msg.getType() == 1)) {
      if (doDEBUG) { cerr<<humanTimeNow()<<" Got: "<<(int)msg.getClass() << " " <<(int)msg.getType() <<" while waiting for ack/nack of " << ubxClass<<" "<<ubxType<<endl; }
      continue;
    }
      
    const auto& payload = msg.getPayload();
    if(payload.size() != 2) {
      if (doDEBUG) { cerr<<"Wrong payload size for ack/nack: "<<payload.size()<<endl; }
      continue;
    }
    if (doDEBUG) { cerr<<humanTimeNow()<<" Got an " << (msg.getType() ? "ack" : "nack")<<" for "<<(int)payload[0] <<" " << (int)payload[1]<<" while waiting for "<<ubxClass<<" " <<ubxType<<endl; }
    
    if(msg.getClass() == 0x05 && msg.getType() == 0x01 && payload[0]==ubxClass && payload[1]==ubxType) {
      return true;
    }
    else if(msg.getClass() == 0x05 && msg.getType() == 0x00 && payload[0]==ubxClass && payload[1]==ubxType) {
      return false;
    }
  }
  throw std::runtime_error("Did not get ACK/NACK response for class "+to_string(ubxClass)+" type "+to_string(ubxType)+" on time");
}

bool sendAndWaitForUBXAckNack(int fd, int seconds, basic_string_view<uint8_t> msg, uint8_t ubxClass, uint8_t ubxType)
{
  for(int n=3; n; --n) {
    writen2(fd, &msg[0], msg.size());
    try {
      return waitForUBXAckNack(fd, seconds, ubxClass, ubxType);
    }
    catch(...) {
      if(n==1)
        throw;
      if (!doSILENT) { cerr<<"Retransmit"<<endl; }
    }
  }
  return false;
}



bool version9 = false;
void enableUBXMessageOnPort(int fd, uint8_t ubxClass, uint8_t ubxType, uint8_t port, uint8_t rate=1)
{
  for(int n=0 ; n < 5; ++n) {
    try {
      basic_string<uint8_t> payload;
      if(version9) {
        payload= basic_string<uint8_t>({ubxClass, ubxType, rate});
      }
      else {
        if(port > 6)
          throw std::runtime_error("Port number out of range (>6)");

        payload.assign({ubxClass, ubxType, 0, 0, 0, 0, 0, 0});
        payload[2+ port]=rate;
      }


     auto msg = buildUbxMessage(0x06, 0x01, payload);
      if(sendAndWaitForUBXAckNack(fd, 2, msg, 0x06, 0x01))
        return;
      else
        throw std::runtime_error("Got NACK enabling UBX message "+to_string((int)ubxClass)+" "+to_string((int)ubxType));
    }
    catch(TimeoutError& te) {
      if (doDEBUG) { cerr<<humanTimeNow()<<" Had "<<n<<"th timeout in enableUBXMessageOnPort"<<endl; }
      continue;
    }
  }
  throw TimeoutError();
}

bool isPresent(string_view fname)
{
  struct stat sb;
  if(stat(&fname[0], &sb) < 0)
    return false;
  return true;
}

bool isCharDevice(string_view fname)
{
  struct stat sb;
  if(stat(&fname[0], &sb) < 0)
    return false;
  return (sb.st_mode & S_IFMT) == S_IFCHR;
}


void readSome(int fd)
{

  for(int n=0; n < 5; ++n) {
    double timeout=1;
    try {
      auto [msg, timestamp] = getUBXMessage(fd, &timeout);
      (void)timestamp;
      if (doDEBUG) { cerr<<humanTimeNow()<<" Read some init: "<<(int)msg.getClass() << " " <<(int)msg.getType() <<endl; }
      if(msg.getClass() == 0x4)
        if (doDEBUG) { cerr<<humanTimeNow()<<" "<<string((char*)msg.getPayload().c_str(), msg.getPayload().size()) <<endl; }
    }
    catch(TimeoutError& te) {
      if (!doSILENT) { cerr<<"Timeout"<<endl; }
    }
  }
  
}

struct termios g_oldtio;

int getCurrentBaudrate(int fd)
{
  struct termios tio;
  if(tcgetattr(fd, &tio)) { /* save current port settings */
    perror("tcgetattr");
    exit(-1);
  }
  return cfgetospeed(&tio);

}

void doTermios(int fd, bool doRTSCTS)
{
  struct termios newtio;
  if(tcgetattr(fd, &g_oldtio)) { /* save current port settings */
    perror("tcgetattr");
    exit(-1);
  }

  bzero(&newtio, sizeof(newtio));                                         
  newtio.c_cflag = CS8 | CLOCAL | CREAD;             
  if (doRTSCTS) {
    if(doDEBUG) { cerr<<humanTimeNow()<<" will enable RTSCTS"<<endl; }
    newtio.c_cflag |= CRTSCTS;
  }
  newtio.c_iflag = IGNPAR;                                                
  newtio.c_oflag = 0;                                                     
  
  /* set input mode (non-canonical, no echo,...) */                       
  newtio.c_lflag = 0;                                                     
    
  newtio.c_cc[VTIME]    = 0;   /* inter-character timer unused */         
  newtio.c_cc[VMIN]     = 4;   /* blocking read until 5 chars received */ 
  
  cfsetspeed(&newtio, g_baudval);
  if(tcsetattr(fd, TCSANOW, &newtio)) {
    perror("tcsetattr");
    exit(-1);
  }
  if (doDEBUG) { cerr<<humanTimeNow()<<" initFD - tty set"<<endl; }
}

int initFD(const char* fname, bool doRTSCTS)
{
  int fd;
  if (doDEBUG) { cerr<<humanTimeNow()<<" initFD()"<<endl; }
  if (!isPresent(fname)) {
    if (doDEBUG) { cerr<<humanTimeNow()<<" initFD - "<<fname<<" - not present"<<endl; }
    throw runtime_error("Opening file "+string(fname));
  }
  if(string(fname) != "stdin" && string(fname) != "/dev/stdin" && isCharDevice(fname)) {
    if (doDEBUG) { cerr<<humanTimeNow()<<" initFD - open("<<fname<<")"<<endl; }
    fd = open(fname, O_RDWR | O_NOCTTY );
    if (fd <0 ) {
      throw runtime_error("Opening file "+string(fname)+": "+strerror(errno));
    }
    if (doDEBUG) { cerr<<humanTimeNow()<<" initFD - open successful"<<endl; }

    if(!g_baudval)
      g_baudval = getCurrentBaudrate(fd);

    doTermios(fd, doRTSCTS);
  }
  else {
    g_fromFile = true;
    
    if (doDEBUG) { cerr<<humanTimeNow()<<" initFD - open("<<fname<<") - from file"<<endl; }
    fd = open(fname, O_RDONLY );
    if(fd < 0)
      throw runtime_error("Opening file "+string(fname));
  }
  return fd;

}

static string format_serial(basic_string<uint8_t> payload)
{
	return fmt::sprintf("%02x%02x%02x%02x%02x",
                            payload[4], payload[5],
                            payload[6], payload[7],
                            payload[8]);
}

// these are four structs to capture Ublox F9P time offset stats
namespace {
struct TIMEGAL
{
  uint32_t itow;
  uint32_t galTow;
  int32_t fGalTow;
  int16_t galWno;
  int8_t leapS;
  uint8_t valid;
  uint32_t tAcc;          
} __attribute__((packed));

struct TIMEBDS
{
  uint32_t itow;
  uint32_t sow;
  int32_t fSow;
  int16_t week;
  int8_t leapS;
  uint8_t valid;
  uint32_t tAcc;          
} __attribute__((packed));

struct TIMEGLO
{
  uint32_t itow;
  uint32_t tod;
  int32_t fTod;
  uint16_t nT;
  uint8_t n4;
  uint8_t valid;
  uint32_t tAcc;          
} __attribute__((packed));

struct TIMEGPS
{
  uint32_t itow;
  int32_t ftow;
  int16_t week;
  int8_t leapS;
  uint8_t valid;
  uint32_t tAcc;          
} __attribute__((packed));

}

// ubxtool device srcid
int main(int argc, char** argv)
{
  time_t starttime=time(0);
  GOOGLE_PROTOBUF_VERIFY_VERSION;

  CLI::App app(program);
    

  bool doGPS{true}, doGalileo{true}, doGlonass{false}, doBeidou{false}, doReset{false}, doWait{true}, doRTSCTS{true}, doSBAS{false};
  bool doFakeFix{false};
  bool doKeepNMEA{false};
  bool doSTDOUT=false;
#ifdef OpenBSD
  doRTSCTS = false;
#endif

  vector<string> destinations;
  string portName;
  int ubxport=3;
  int baudrate=0;
  int newbaudrate=0;
  unsigned int fuzzPositionMeters=0;
  string owner;
  string remark;
  bool doCompress=true;
  app.add_option("--destination,-d", destinations, "Send output to this IPv4/v6 address");
  app.add_flag("--wait", doWait, "Wait a bit, do not try to read init messages");
  //  app.add_flag("--compress,-z", doCompress, "Use compressed protocol for network transmission");
  app.add_flag("--reset", doReset, "Reset UBX device");
  app.add_flag("--beidou,-c", doBeidou, "Enable BeiDou reception");
  app.add_flag("--gps,-g", doGPS, "Enable GPS reception");
  app.add_flag("--glonass,-r", doGlonass, "Enable Glonass reception");
  app.add_flag("--galileo,-e", doGalileo, "Enable Galileo reception");
  app.add_flag("--sbas,-s", doSBAS, "Enable SBAS (EGNOS/WAAS/etc) reception");
  app.add_option("--rtscts", doRTSCTS, "Set hardware handshaking");
  app.add_flag("--stdout", doSTDOUT, "Emit output to stdout");
  auto pn = app.add_option("--port,-p", portName, "Device or file to read serial from");
  app.add_option("--station", g_srcid, "Station id");
  app.add_option("--ubxport,-u", ubxport, "UBX port to enable messages on (usb=3)");
  app.add_option("--baud,-b", baudrate, "Baudrate for serial connection");
  app.add_option("--newbaud,-n", newbaudrate, "Attempt to change to this baudrate for serial connection");  
  app.add_flag("--keep-nmea,-k", doKeepNMEA, "Don't disable NMEA");
  app.add_flag("--fake-fix", doFakeFix, "Inject locally generated fake fix data");
  app.add_option("--fuzz-position,-f", fuzzPositionMeters, "Fuzz position by this many meters");
  app.add_option("--owner,-o", owner, "Name/handle/nick of owner/operator");
  app.add_option("--remark", remark, "Remark for this station");
  
  int surveyMinSeconds = 0;
  int surveyMinCM = 0;
  bool doSurveyReset=false;
  app.add_option("--survey-min-seconds", surveyMinSeconds, "Survey minimally this amount of seconds");
  app.add_option("--survey-min-cm", surveyMinCM, "Survey until accuracy is better (lower) than this setting");
  app.add_flag("--survey-reset", doSurveyReset, "Reset the Surveyed-in state");
  app.add_flag("--debug", doDEBUG, "Display debug information");  
  app.add_flag("--logfile", doLOGFILE, "Create logfile");  
  app.add_flag("--silent", doSILENT, "Supress non essential logging");  
  app.add_flag("--version", doVERSION, "show program version and copyright");
  try {
    app.parse(argc, argv);
  } catch(const CLI::Error &e) {
    return app.exit(e);
  }

  if(doVERSION) {
    showVersion(program, g_gitHash);
    exit(0);
  }

  if(! *pn) {
    cerr<<"Error: you must provide the --port"<<endl;
    exit(1);
  }

  if(baudrate)
    g_baudval = getBaudrate(baudrate);

  if(!(doGPS || doGalileo || doGlonass || doBeidou)) {
    cerr<<"Error: Enable at least one of --gps, --galileo, --glonass, --beidou"<<endl;
    return EXIT_FAILURE;
  }

  if(doDEBUG && doSILENT) {
    cerr<<"--silent and --debug are mutually exclusive."<<endl;
    return EXIT_FAILURE;
  }

  signal(SIGPIPE, SIG_IGN);
  NMMSender ns;
  ns.d_debug = doDEBUG;
  for(const auto& s : destinations) {
    auto res=resolveName(s, true, true);
    if(res.empty()) {
      cerr<<"Error: Unable to resolve '"<<s<<"' as destination for data, exiting"<<endl;
      exit(EXIT_FAILURE);
    }
    ns.addDestination(s); // ComboAddress(s, 29603));
  }
  if(doSTDOUT)
    ns.addDestination(1);
  
  int fd = initFD(portName.c_str(), doRTSCTS);
  if(!baudrate)
    baudrate = getBaudrateFromSymbol(g_baudval);

  if(doFakeFix) // hack
    version9 = true;
  bool m8t = false;

  string hwversion;
  string swversion;
  string mods;
  string serialno;
  
  if(!g_fromFile) {
    bool doInit = true;
    if(doInit) {
      if(doWait)
        sleep(2);
      else
        readSome(fd);
      
      std::basic_string<uint8_t> msg;
      if(doReset) {
        if (doDEBUG) { cerr<<humanTimeNow()<<" Sending a soft reset"<<endl; }
        msg = buildUbxMessage(0x06, 0x04, {0x00, 0x00, 0x01, 0x00}); // soft reset
        writen2(fd, msg.c_str(), msg.size());
        usleep(100000);
        close(fd);
        for(int n=0 ; n< 20; ++n) {
          if (doDEBUG) { cerr<<humanTimeNow()<<" Waiting for device to come back"<<endl; }
          try {
            fd = initFD(portName.c_str(), doRTSCTS);
            readSome(fd);          
          }
          catch(...)
            {
              if (doDEBUG) { cerr<<humanTimeNow()<<" Not yet back"<<endl; }
              usleep(400000);
              continue;
            }
          break;
        }
      }


      msg = buildUbxMessage(0x0a, 0x04, {});

      if (doDEBUG) { cerr<<humanTimeNow()<<" Sending version query"<<endl; }
      UBXMessage um1=sendAndWaitForUBX(fd, 1, msg, 0x0a, 0x04); // ask for version
      swversion = (const char*)um1.getPayload().c_str();
      hwversion = (const char*)um1.getPayload().c_str()+30;
      cerr<<humanTimeNow()<<" swVersion: "<<swversion<<endl;
      cerr<<humanTimeNow()<<" hwVersion: "<<hwversion<<endl;

      for(unsigned int n=0; 40+30*n < um1.getPayload().size(); ++n) {
        string line = (const char*)um1.getPayload().c_str() + 40 +30*n;
        cerr<<humanTimeNow()<<" Extended info: "<<line <<endl;
        
        if(line.find("F9") != string::npos)
          version9=true;

        if(line.find("M8T") != string::npos) {
          m8t=true;
        }

        if(line.find("MOD=") != string::npos)
          mods += line.substr(4);
        
        // timing: MOD=NEO-M8T-0
        
      }
      if (doDEBUG && m8t) { cerr<<humanTimeNow()<<" Detected timing module"<<endl; }
      if (doDEBUG) { cerr<<humanTimeNow()<<" Sending serial number query"<<endl; }
      msg = buildUbxMessage(0x27, 0x03, {}); // UBX-SEC-UNIQID
      um1=sendAndWaitForUBX(fd, 1, msg, 0x27, 0x03); // ask for serial
      serialno = format_serial(um1.getPayload());
      
      cerr<<humanTimeNow()<<" Serial number "<< serialno <<endl;

      if(version9)
        cerr<<humanTimeNow()<<" Detected version U-Blox 9"<<endl;
      usleep(50000);
      if (doDEBUG) { cerr<<humanTimeNow()<<" Sending GNSS query"<<endl; }
      msg = buildUbxMessage(0x06, 0x3e, {});

      um1=sendAndWaitForUBX(fd, 1, msg, 0x06, 0x3e); // query GNSS
      auto payload = um1.getPayload();
      if (doDEBUG) {
        cerr<<humanTimeNow()<<" GNSS status, got " << (int)payload[3]<<" rows:"<<endl;
        for(uint8_t n = 0 ; n < payload[3]; ++n) {
          cerr<<humanTimeNow()<<" GNSSID "<<(int)payload[4+8*n]<<" enabled "<<(int)payload[8+8*n]<<" minTrk "<< (int)payload[5+8*n] <<" maxTrk "<<(int)payload[6+8*n]<<" " << (int)payload[8+8*n]<<" " << (int)payload[9+8*n] << " " <<" " << (int)payload[10+8*n]<<" " << (int)payload[11+8*n]<<endl;
        }
      }

      try {
      if(waitForUBXAckNack(fd, 2, 0x06, 0x3e)) {
        if (doDEBUG) { cerr<<humanTimeNow()<<" Got ACK for our poll of GNSS settings"<<endl; }
      }
      }catch(...) {
        cerr<<"Got timeout waiting for ack of poll, no problem"<<endl;
      }
      if(!version9) {
        //                                  ver   RO   maxch cfgs
        msg = buildUbxMessage(0x06, 0x3e, {0x00, 0x00, 0xff, 0x06,
              //                            GPS   min  max   res   x1         x2    x3,   x4
              0x00, 0x04, 0x08, 0,  doGPS,    0x00, 0x01, 0x00,
              //                            SBAS  min  max   rex   x1       x2    x3    x4
              0x01, 0x03, 0x04, 0,   doSBAS,  0x00, 0x01, 0x00,
              //                            BEI   min  max   res   x1       x2    x3,   x4
              0x03, 0x04, 0x08, 0,  doBeidou, 0x00, 0x01, 0x00,
              //                            ???   min  max   res   x1   x2    x3,   x4
              0x05, 0x04, 0x08, 0,  0, 0x00, 0x01, 0x00,
            
              //                            GAL   min  max   res   x1   x2    x3,   x4
              0x02, 0x08, 0x0A, 0,  doGalileo, 0x00, 0x01, 0x00,
              //                            GLO   min  max   res   x1   x2    x3,   x4
              0x06, 0x06, 0x08, 0,  doGlonass, 0x00, 0x01, 0x00

              });
      
        if (doDEBUG) { cerr<<humanTimeNow()<<" Sending GNSS setting, GPS: "<<doGPS<<", Galileo: "<<doGalileo<<", BeiDou: "<<doBeidou<<", GLONASS: "<<doGlonass<<", SBAS: "<<doSBAS<<endl; }
      
        if(sendAndWaitForUBXAckNack(fd, 2, msg, 0x06, 0x3e)) { // GNSS setting
          if (doDEBUG) { cerr<<humanTimeNow()<<" Got ack on GNSS setting"<<endl; }
        }
        else {
          cerr<<humanTimeNow()<<" Error: Got nack on GNSS setting"<<endl;
          exit(-1);
        }
      }
      else { // UBX-CFG-VALSET
        //                                 vers  ram   res   res    
        msg = buildUbxMessage(0x06, 0x8a, {0x00, 0x01, 0x00, 0x00,
              0x1f,0x00,0x31,0x10, doGPS,   // 
              0x01,0x00,0x31,0x10, doGPS,
              0x03,0x00,0x31,0x10, doGPS,

              0x21,0x00,0x31,0x10, doGalileo,
              0x07,0x00,0x31,0x10, doGalileo,
              0x0a,0x00,0x31,0x10, doGalileo,

              0x22,0x00,0x31,0x10, doBeidou,
              0x0d,0x00,0x31,0x10, doBeidou,
              0x0e,0x00,0x31,0x10, doBeidou,

              0x25,0x00,0x31,0x10, doGlonass,
              0x18,0x00,0x31,0x10, doGlonass,
              0x1a,0x00,0x31,0x10, doGlonass
              });

        if (doDEBUG) { cerr<<humanTimeNow()<<" Sending F9P GNSS setting, GPS: "<<doGPS<<", Galileo: "<<doGalileo<<", BeiDou: "<<doBeidou<<", GLONASS: "<<doGlonass<<", SBAS: "<<doSBAS<<endl; }
        
        if(sendAndWaitForUBXAckNack(fd, 2, msg, 0x06, 0x8a)) { // GNSS setting, F9 stylee
          if (doDEBUG) { cerr<<humanTimeNow()<<" Got ack on F9P GNSS setting"<<endl; }
        }
        else {
          cerr<<humanTimeNow()<<" Error: Got nack on F9P GNSS setting"<<endl;
          exit(-1);
        }
        /* VALSET
        0x20 91 02 32 = 
        */

        msg = buildUbxMessage(0x06, 0x8a, {0x00, 0x01, 0x00, 0x00,
              0x07, 0x00, 0x91, 0x20, 1,
              0x32, 0x02, 0x91, 0x20, 1});
        if(sendAndWaitForUBXAckNack(fd, 2, msg, 0x06, 0x8a)) { // msg cfg F9P
          if (doDEBUG) { cerr<<humanTimeNow()<<" Got ack on F9P UART1 setting"<<endl; }
        }
        else {
          cerr<<humanTimeNow()<<" Error: Got nack on F9P UART1 setting"<<endl;
          exit(-1);
        }
      }
      if(m8t) {
        cerr<<humanTimeNow()<<" Sending TMODE2 status query"<<endl;
        msg = buildUbxMessage(0x06, 0x3d, {});      
        um1=sendAndWaitForUBX(fd, 1, msg, 0x06, 0x3d); // query TMODE2
        auto tmodepayload = um1.getPayload();
        cerr<<humanTimeNow()<<" TMODE2 status, mode " << (int)tmodepayload[0] << endl;

        try {
          if(waitForUBXAckNack(fd, 2, 0x06, 0x3d)) {
            if (doDEBUG) { cerr<<humanTimeNow()<<" Got ACK for our poll of TMODE2"<<endl; }
          }
          }catch(...) {
            cerr<<"Got timeout waiting for ack of poll, no problem"<<endl;
        }
      }
      if(doSurveyReset) {
          uint8_t cmd = 0x3d;                   //  vers  res   survey ign      
          auto msg = buildUbxMessage(0x06, cmd, 
          { 0,0,0,0, // survey-in, res, flag1, flag2
            0,0,0,0, // x
            0,0,0,0, // y
            0,0,0,0, // z
            0,0,0,0, // fixed position accuracy
            0,0,0,0,
            0,0,0,0
            });
        cerr<<humanTimeNow()<<" Sending survey-reset commmand"<<endl;
      
        if(sendAndWaitForUBXAckNack(fd, 2, msg, 0x06, cmd)) { 
          if (doDEBUG) { cerr<<humanTimeNow()<<" Got ack on survey-reset"<<endl; }
        }
        else {
          cerr<<humanTimeNow()<<" Error: Got nack on survey-reset"<<endl;
          exit(-1);
        }     
        exit(0);
      }
      if(surveyMinSeconds || surveyMinCM) {
        uint32_t minSecondsVal = surveyMinSeconds;
        uint32_t minCentimetersVal;
        if(surveyMinCM==0)
          minCentimetersVal = 10000000; // 100km
        else 
          minCentimetersVal = surveyMinCM * 100;
        uint8_t* ptrSeconds = (uint8_t*)&minSecondsVal, *ptrCent= (uint8_t*)&minCentimetersVal;
        uint8_t cmd;
        std::basic_string<uint8_t> msg;
        if(version9) {        
          cmd = 0x8a;
          msg = buildUbxMessage(0x06, cmd, {0x00, 0x01, 0x00, 0x00,
              0x01,0x00,0x03,0x20, 1, // survey in mode
              // min survey time:  
              0x10,0x00,0x03,0x40, ptrSeconds[0], ptrSeconds[1], ptrSeconds[2], ptrSeconds[3], 
              0x11,0x00,0x03,0x40, ptrCent[0], ptrCent[1], ptrCent[2], ptrCent[3]
              });
        }
        else {
          minCentimetersVal /= 10;
          cmd = 0x3d;                   
          msg = buildUbxMessage(0x06, cmd, 
          { 1,0,0,0, // survey-in, res, flag1, flag2
            0,0,0,0, // x
            0,0,0,0, // y
            0,0,0,0, // z
            0,0,0,0, // fixed position accuracy
            ptrSeconds[0], ptrSeconds[1], ptrSeconds[2], ptrSeconds[3], 
            ptrCent[0], ptrCent[1], ptrCent[2], ptrCent[3]
            });
        }
    
        cerr<<humanTimeNow()<<" Sending survey-in commmand"<<endl;
      
        if(sendAndWaitForUBXAckNack(fd, 2, msg, 0x06, cmd)) { 
          if (doDEBUG) { cerr<<humanTimeNow()<<" Got ack on survey-in"<<endl; }
        }
        else {
          cerr<<humanTimeNow()<<" Error: Got nack on survey-in"<<endl;
          exit(-1);
        }
      }
      if(doSBAS) {
        //                                 "on" "*.*"  ign   scanmode--------------------
        msg = buildUbxMessage(0x06, 0x16, {0x01, 0x07, 0x03, 0x00, 0x00, 0x00, 0x00, 0x00}); // enable SBAS

        if(sendAndWaitForUBXAckNack(fd, 10, msg, 0x06, 0x16)) { // enable SBAS
          if (doDEBUG) { cerr<<humanTimeNow()<<" Got ack on SBAS setting"<<endl; }
        }
        else {
          cerr<<humanTimeNow()<<" Error: Got nack on SBAS setting"<<endl;
          exit(-1);
        }
      }
      

      if(!doKeepNMEA || (newbaudrate && newbaudrate != baudrate)) {
        if (doDEBUG) { cerr<<humanTimeNow()<<" Changing port settings (newbaudrate="<<newbaudrate<<", baudrate="<<baudrate<<", keepNMEA="<<doKeepNMEA<<")"<<endl; }
        
        uint8_t outproto = 0x01;
        if(doKeepNMEA)
          outproto += 0x02;
          
        if(ubxport == 1 || ubxport == 2) {
          /* Ublox UART[1] or UART2 port, so encode baudrate and serial settings */
          int actbaud = newbaudrate ? newbaudrate : baudrate;
          msg = buildUbxMessage(0x06, 0x00, {(unsigned char)(ubxport),0x00,0x00,0x00,
            0xC0 /* 8 bit */,
            0x08 /* No parity */,
            0x00,0x00,
            (unsigned char)((actbaud) & 0xFF),
            (unsigned char)((actbaud>>8) & 0xFF),
            (unsigned char)((actbaud>>16) & 0xFF),
            (unsigned char)((actbaud>>24) & 0xFF),
            // in in   out      out      // 0x01 = ublox, 0x02 = nmea
            0x03,0x00,outproto,0x00,
            // flags   res  res
            0x00,0x00,0x00,0x00
          });
        }
        else {                              //   port                 res   tx-ready  res  res  res  res  res  res  res res   in   in     out  out  res   res  res res 
          msg = buildUbxMessage(0x06, 0x00, {(unsigned char)(ubxport),0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x03,0x00,outproto,0x00,0x00,0x00,0x00,0x00});
        }

        if(newbaudrate && newbaudrate != baudrate) {
          for(int n=0; n < 3; ++n) {
            cerr<<humanTimeNow()<<" Sending new baudrate "<<n<<".. "<<endl;
            writen2(fd, &msg[0], msg.size());
            // there won't be an ack
            usleep(100000);
          }
          g_baudval = getBaudrate(newbaudrate);
          doTermios(fd, false);          
        }
        else {
          if(sendAndWaitForUBXAckNack(fd, 10, msg, 0x06, 0x00)) { // disable NMEA
            if (doDEBUG) { cerr<<humanTimeNow()<<" ACK for new port/protocol settings"<<endl; }
          }
          else {
            if (doDEBUG) { cerr<<humanTimeNow()<<" Got NACK for port/protocol settings"<<endl; }
          }
        }
      }
      if (doDEBUG) { cerr<<humanTimeNow()<<" Polling port settings"<<endl; } // UBX-CFG-PRT, 0x03 == USB
      msg = buildUbxMessage(0x06, 0x00, {(unsigned char)(ubxport)});
      
      UBXMessage um=sendAndWaitForUBX(fd, 4, msg, 0x06, 0x00); // UBX-CFG-PRT
      if (doDEBUG) {
        cerr<<humanTimeNow()<<" Protocol settings on port: "<<endl;
        for(const auto& c : um.getPayload())
          cerr<<(int)c<< " ";
        cerr<<endl;
      }
      try {
        if(waitForUBXAckNack(fd, 2, 0x06, 0x00)) {
          if (doDEBUG) { cerr<<humanTimeNow()<<" Got ACK for our poll of port protocol settings"<<endl; }
        }
      }catch(...) {
        cerr<<"Got timeout waiting for ack of port protocol poll, no problem"<<endl;
      }


      if(mods.find("NEO-M8P") ==string::npos) {
        if (doDEBUG) { cerr<<humanTimeNow()<<" Enabling UBX-RXM-RLM"<<endl; } // SAR
        enableUBXMessageOnPort(fd, 0x02, 0x59, ubxport); // UBX-RXM-RLM
      }

      if (doDEBUG) { cerr<<humanTimeNow()<<" Enabling UBX-MON-HW"<<endl; } // SAR
      enableUBXMessageOnPort(fd, 0x0A, 0x09, ubxport, 16); // UBX-MON-HW

      
      if(version9) {
        if (doDEBUG) { cerr<<humanTimeNow()<<" Enabling UBX-NAV-SVIN"<<endl; } // Survey-in results
        enableUBXMessageOnPort(fd, 0x01, 0x3b, ubxport, 2); 
      }
      else if(m8t) {
        if (doDEBUG) { cerr<<humanTimeNow()<<" Enabling UBX-TIM-SVIN"<<endl; } // Survey-in results
        enableUBXMessageOnPort(fd, 0x0d, 0x04, ubxport, 2);       
      }

      if(mods.find("NEO-M9N") == string::npos) {
        if (doDEBUG) { cerr<<humanTimeNow()<<" Enabling UBX-RXM-RAWX"<<endl; } // RF doppler
        enableUBXMessageOnPort(fd, 0x02, 0x15, ubxport, 8); // RXM-RAWX
      }

      if (doDEBUG) { cerr<<humanTimeNow()<<" Enabling UBX-NAV-CLOCK"<<endl; } // clock details
      enableUBXMessageOnPort(fd, 0x01, 0x22, ubxport, 16); // UBX-NAV-CLOCK

      if(1) {
        if (doDEBUG) { cerr<<humanTimeNow()<<" Enabling/disabling UBX-NAV-TIMEGPS"<<endl; } // GPS time solution
        enableUBXMessageOnPort(fd, 0x01, 0x20, ubxport, doGPS ? 16 : 0); // UBX-NAV-TIMEGPS

        if (doDEBUG) { cerr<<humanTimeNow()<<" Enabling/disabling "<< doGlonass<< " UBX-NAV-TIMEGLO"<<endl; } // GLONASS time solution
        enableUBXMessageOnPort(fd, 0x01, 0x23, ubxport, doGlonass ? 16 : 0); // UBX-NAV-TIMEGLO
	
        if (doDEBUG) { cerr<<humanTimeNow()<<" Enabling/disabling UBX-NAV-TIMEBDS"<<endl; } // Beidou time solution
        enableUBXMessageOnPort(fd, 0x01, 0x24, ubxport, doBeidou ? 16 : 0); // UBX-NAV-TIMEBDS

        if(mods.find("NEO-M8P") ==string::npos) { 
          if (doDEBUG) { cerr<<humanTimeNow()<<" Enabling/disabling UBX-NAV-TIMEGAL"<<endl; } // Galileo time solution
          enableUBXMessageOnPort(fd, 0x01, 0x25, ubxport, doGalileo ? 16 : 0); // UBX-NAV-TIMEGAL
        }
      }
            
      if(!version9 && !m8t && !fuzzPositionMeters) {
        if (doDEBUG) { cerr<<humanTimeNow()<<" Enabling debugging data"<<endl; } // RF doppler
        enableUBXMessageOnPort(fd, 0x03, 0x10, ubxport, 4);
      }
      else
        enableUBXMessageOnPort(fd, 0x03, 0x10, ubxport, 0);
        
      if (doDEBUG) { cerr<<humanTimeNow()<<" Enabling UBX-RXM-SFRBX"<<endl; } // raw navigation frames
      enableUBXMessageOnPort(fd, 0x02, 0x13, ubxport); // SFRBX
      
      if (doDEBUG) { cerr<<humanTimeNow()<<" Enabling UBX-NAV-POSECEF"<<endl; } // position
      enableUBXMessageOnPort(fd, 0x01, 0x01, ubxport, 8); // POSECEF

      if(version9)  {
        if (doDEBUG) { cerr<<humanTimeNow()<<" Enabling UBX-NAV-SIG"<<endl; }  // satellite reception details
        enableUBXMessageOnPort(fd, 0x01, 0x43, ubxport, 8); // NAV-SIG
        /*
        if (doDEBUG) { cerr<<humanTimeNow()<<" Enabling UBX-RXM-MEASX"<<endl; }  // satellite reception details
        enableUBXMessageOnPort(fd, 0x02, 0x14, ubxport, 1); // RXM-MEASX
        */
      }
      else {
        if (doDEBUG) { cerr<<humanTimeNow()<<" Enabling UBX-NAV-SAT"<<endl; }  // satellite reception details
        enableUBXMessageOnPort(fd, 0x01, 0x35, ubxport, 8); // NAV-SAT
      }
      
      if (doDEBUG) { cerr<<humanTimeNow()<<" Enabling UBX-NAV-PVT"<<endl; } // position, velocity, time fix
      enableUBXMessageOnPort(fd, 0x01, 0x07, ubxport, 1); // NAV-PVT we use this to get timing
    }
  }
  
  /* goal: isolate UBX messages, ignore everyting else.
     The challenge is that we might sometimes hit the 0xb5 0x62 marker
     in the middle of a message, which will cause us to possibly jump over valid messages 

     This program sits on the serial link and therefore has the best timestamps.
     At least Galileo messages sometimes rely on the timestamp-of-receipt, but the promise
     is that such messages are sent at whole second intervals.

     We therefore perform a layering violation and peer into the message to see
     what timestamp it claims to have, so that we can set subsequent timestamps correctly.
  */

  int curCycleTOW{-1}; // means invalid
  ns.d_compress = doCompress;
  ns.launch();
  
  cerr<<humanTimeNow()<<" Entering main loop"<<endl;

  struct TIMEXState
  {
    TIMEGAL gal;
    TIMEGPS gps;
    TIMEGLO glo;
    TIMEBDS bds;
    bool doGlonass, doGalileo, doBeidou, doGPS;
    void transmitIfComplete(NMMSender& ns)
    {
      vector<int> itows;
      if(doGlonass)
        itows.push_back(glo.itow);
      if(doGalileo)
        itows.push_back(gal.itow);
      if(doBeidou)
        itows.push_back(bds.itow);
      if(doGPS)
        itows.push_back(gps.itow);

      if(itows.empty())
        return;
      if(itows[0] == 0)
        return;
        
      for(const auto& itow : itows)
        if(itow != itows[0])
           return;
        
      NavMonMessage nmm;
      nmm.set_sourceid(g_srcid);
      nmm.set_localutcseconds(g_gnssutc.tv_sec);
      nmm.set_localutcnanoseconds(g_gnssutc.tv_nsec);
      
      nmm.set_type(NavMonMessage::TimeOffsetType);
      nmm.mutable_to()->set_itow(gps.itow);

      NavMonMessage::GNSSOffset* no;
      if(doGPS) {
        no = nmm.mutable_to()->add_offsets();
        no->set_gnssid(0);
        no->set_offsetns(gps.ftow);
        no->set_tacc(gps.tAcc);
        no->set_tow(gps.itow); // this is for consistency
        no->set_leaps(gps.leapS);
        no->set_wn(gps.week);
        no->set_valid(gps.valid);
      }
      
      if(doGalileo) {
        no = nmm.mutable_to()->add_offsets();
        no->set_gnssid(2);
        no->set_offsetns(gal.fGalTow);
        no->set_tacc(gal.tAcc);
        no->set_leaps(gal.leapS);
        no->set_wn(gal.galWno);
        no->set_valid(gal.valid);
        no->set_tow(gal.galTow);
      }
      
      if(doBeidou) {
        no = nmm.mutable_to()->add_offsets();
        no->set_gnssid(3);
        no->set_offsetns(bds.fSow);
        no->set_tacc(bds.tAcc);
        no->set_leaps(bds.leapS);
        no->set_wn(bds.week);
        no->set_valid(bds.valid);
        no->set_tow(bds.sow);
      }
      
      if(doGlonass) {
        no = nmm.mutable_to()->add_offsets();
        no->set_gnssid(6);
        no->set_offsetns(glo.fTod);
        no->set_tacc(glo.tAcc);
        no->set_nt(glo.nT);
        no->set_n4(glo.n4);
        no->set_valid(glo.valid);
        no->set_tow(glo.tod);
      }
      ns.emitNMM(nmm);
      gal.itow = 0;
      gps.itow = 0;
      glo.itow = 0;
      bds.itow = 0;
    }
  } tstate;
  
  tstate.doGPS = doGPS; tstate.doGalileo = doGalileo; tstate.doGlonass = doGlonass;
  tstate.doBeidou = doBeidou;
  
  for(;;) {
    try {
      auto [msg, timestamp] = getUBXMessage(fd, nullptr);
      (void)timestamp;
      auto payload = msg.getPayload();
      
      if(msg.getClass() == 0x01 && msg.getType() == 0x07) {  // UBX-NAV-PVT
        struct PVT
        {
          uint32_t itow;
          uint16_t year;
          uint8_t month; // jan = 1
          uint8_t day;
          uint8_t hour; // 24
          uint8_t min;
          uint8_t sec;
          uint8_t valid;
          uint32_t tAcc;
          int32_t nano;
          uint8_t fixtype;
          uint8_t flags;
          uint8_t flags2;
          uint8_t numsv;
          //      1e-7       mm       mm
          int32_t lon, lat, height, hMSL;
          //        mm     mm
          uint32_t hAcc, vAcc;
          //      mm/s   mm/s  mm/s  mm/s
          int32_t velN, velE, velD, gSpeed; // millimeters
          
        } __attribute__((packed));
        PVT pvt;
        
        memcpy(&pvt, &payload[0], sizeof(pvt));
        //        cerr << "Ground speed: "<<pvt.gSpeed<<", "<<pvt.velN<<" "<<pvt.velE<<" "<<pvt.velD << endl;
        
        g_fixtype = pvt.fixtype;
        g_speed = pvt.gSpeed / 1000.0;
        
        struct tm tm;
        memset(&tm, 0, sizeof(tm));
        tm.tm_year = pvt.year - 1900;
        tm.tm_mon = pvt.month - 1;
        tm.tm_mday = pvt.day;
        tm.tm_hour = pvt.hour;
        tm.tm_min = pvt.min;
        tm.tm_sec = pvt.sec;

        uint32_t satt = timegm(&tm);
        //        double satutc = timegm(&tm) + pvt.nano/1000000000.0; // negative is no problem here
        if(pvt.nano < 0) {
          pvt.sec--;
          satt--;
          pvt.nano += 1000000000;
        }
        if(!g_gnssutc.tv_sec) {

          if (doDEBUG) { cerr<<humanTimeNow()<<" Got initial timestamp: "<<humanTime(satt)<<endl; }
        }
        g_gnssutc.tv_sec = satt;
        g_gnssutc.tv_nsec = pvt.nano;
        continue;
      }

      if(!doFakeFix) {
        if(!g_gnssutc.tv_sec) {
          
          if (doDEBUG) { cerr<<humanTimeNow()<<" Ignoring message with class "<<(int)msg.getClass()<< " and type "<< (int)msg.getType()<<": have not yet received a timestamp"<<endl; }
          continue;
        }
      }
      else {
        auto oldtime = g_gnssutc;
        clock_gettime(CLOCK_REALTIME, &g_gnssutc);
        if(oldtime.tv_sec != g_gnssutc.tv_sec) {
          NavMonMessage nmm;
          nmm.set_type(NavMonMessage::ObserverPositionType);
          nmm.set_localutcseconds(g_gnssutc.tv_sec);
          nmm.set_localutcnanoseconds(g_gnssutc.tv_nsec);
          nmm.set_sourceid(g_srcid);
          //      3924698.1158 301124.8036 5001904.9952
          nmm.mutable_op()->set_x(3924698.1158);
          nmm.mutable_op()->set_y(301124.8036);
          nmm.mutable_op()->set_z(5001904.9952);
          nmm.mutable_op()->set_acc(3.14);
          ns.emitNMM( nmm);
        }
      }

      if(msg.getClass() == 0x27 && msg.getType() == 0x03) { // serial
        serialno = format_serial(payload);
        if(doDEBUG)
          cerr<<humanTimeNow()<<" Serial number from stream "<< serialno <<endl;
      }
      else       if(msg.getClass() == 0x02 && msg.getType() == 0x15) {  // RAWX, the doppler stuff
        //        if (doDEBUG) { cerr<<humanTimeNow()<<" Got "<<(int)payload[11] <<" measurements "<<endl; }
        double rcvTow;
        memcpy(&rcvTow, &payload[0], 8);
        uint16_t rcvWn = payload[8] + 256*payload[9];
        bool clkReset = payload[12] & 0x02;
        
        for(int n=0 ; n < payload[11]; ++n) {
          double prMes;
          double cpMes;
          float doppler;
          
          memcpy(&prMes, &payload[16+32*n], 8);
          memcpy(&cpMes, &payload[24+32*n], 8);
          memcpy(&doppler, &payload[32+32*n], 4);
          int gnssid = payload[36+32*n];
          int sv = payload[37+32*n];
          int sigid=0;
          if(version9) {
            sigid = payload[38+32*n];
            if(gnssid == 2 && sigid ==6)  // they separate out I and Q, but the rest of UBX doesn't
              sigid = 5;                  // so map it back
            if(gnssid == 2 && sigid ==0)  // they separate out I and Q, but the rest of UBX doesn't
              sigid = 1;                  // so map it back
          }
          else if(gnssid==2) { // version 8 defaults galileo to E1B
            sigid = 1;
          }

          
          uint16_t locktimems;
          memcpy(&locktimems, &payload[40+32*n], 2);
          uint8_t cno = payload[42+32*n];
          uint8_t prStddev = payload[43+32*n] & 0xf;
          uint8_t cpStddev = payload[44+32*n] & 0xf;
          uint8_t doStddev = payload[45+32*n] & 0xf;
          uint8_t trkStat = payload[46+32*n] & 0xf;

          NavMonMessage nmm;
          nmm.set_type(NavMonMessage::RFDataType);
          nmm.set_localutcseconds(g_gnssutc.tv_sec);
          nmm.set_localutcnanoseconds(g_gnssutc.tv_nsec);
          nmm.set_sourceid(g_srcid); 

          nmm.mutable_rfd()->set_gnssid(gnssid);
          nmm.mutable_rfd()->set_gnsssv(sv);
          nmm.mutable_rfd()->set_sigid(sigid);
          nmm.mutable_rfd()->set_rcvtow(rcvTow);
          nmm.mutable_rfd()->set_rcvwn(rcvWn);
          nmm.mutable_rfd()->set_doppler(doppler);
          nmm.mutable_rfd()->set_carrierphase(cpMes);
          nmm.mutable_rfd()->set_pseudorange(prMes);

          nmm.mutable_rfd()->set_prstd(ldexp(0.01, prStddev));
          nmm.mutable_rfd()->set_dostd(ldexp(0.002, doStddev));
          nmm.mutable_rfd()->set_cpstd(cpStddev*0.4);
          nmm.mutable_rfd()->set_locktimems(locktimems);
          nmm.mutable_rfd()->set_cno(cno);

          nmm.mutable_rfd()->set_prvalid(trkStat & 1);
          nmm.mutable_rfd()->set_cpvalid(trkStat & 2);
          nmm.mutable_rfd()->set_halfcycvalid(trkStat & 4);
          nmm.mutable_rfd()->set_subhalfcyc(trkStat & 8);

          nmm.mutable_rfd()->set_clkreset(clkReset);
          ns.emitNMM( nmm);
        }
      }
      else if(msg.getClass() == 0x01 && msg.getType() == 0x01) {  // POSECF
        struct pos
        {
          uint32_t iTOW;
          int32_t ecefX;
          int32_t ecefY;
          int32_t ecefZ;
          uint32_t pAcc;
        };
        pos p;
        memcpy(&p, payload.c_str(), sizeof(pos));
        if(fuzzPositionMeters) {
          p.ecefX -= (p.ecefX % (fuzzPositionMeters*100));
          p.ecefY -= (p.ecefY % (fuzzPositionMeters*100));
          p.ecefZ -= (p.ecefZ % (fuzzPositionMeters*100));
        }

        /*
        if (doDEBUG) {
          cerr<<humanTimeNow()<<" Position: ("<< p.ecefX / 100000.0<<", "
            << p.ecefY / 100000.0<<", "
            << p.ecefZ / 100000.0<<") +- "<<p.pAcc<<" cm"<<endl;
        }
        */

//        g_ourpos = {p.ecefX/100.0, p.ecefY/100.0, p.ecefZ/100.0};
        
        NavMonMessage nmm;
        nmm.set_type(NavMonMessage::ObserverPositionType);
        nmm.set_localutcseconds(g_gnssutc.tv_sec);
        nmm.set_localutcnanoseconds(g_gnssutc.tv_nsec);
        nmm.set_sourceid(g_srcid);
        
        nmm.mutable_op()->set_x(p.ecefX /100.0);
        nmm.mutable_op()->set_y(p.ecefY /100.0);
        nmm.mutable_op()->set_z(p.ecefZ /100.0);
        nmm.mutable_op()->set_acc(p.pAcc /100.0);
        if(g_speed >= 0.0)
          nmm.mutable_op()->set_groundspeed(g_speed);

        ns.emitNMM( nmm);
      }
      else if(msg.getClass() == 2 && msg.getType() == 0x13) {  // SFRBX
                  
        // order: 2, 4, 6, 7/9, 8/10, 0, 0, 0, 0, 0, 1, 3, 5, 0, 0
        //              *             *  *  *  *  *        * 
        // tow    
        
        try {
          pair<int,int> id = make_pair(payload[0], payload[1]);
          int sigid = payload[2];
          static set<tuple<int,int,int>> svseen;
          static time_t lastStat;
          svseen.insert({id.first, id.second, payload[2]});

          if(time(0)- lastStat > 30) {
            if (!doSILENT) { 
              cerr<<humanTimeNow()<<" src "<<g_srcid<< " (fix: "<<g_fixtype<<") currently receiving: ";
              for(auto& s : svseen) {
                cerr<<get<0>(s)<<","<<get<1>(s)<<"@"<<get<2>(s)<<" ";
              }
              cerr<<endl;
            }
            lastStat = time(0);
            svseen.clear();
          }
          
          if(id.first == 0 && !sigid) { // classic GPS
            NavMonMessage nmm;
            nmm.set_type(NavMonMessage::GPSInavType);
            nmm.set_localutcseconds(g_gnssutc.tv_sec);
            nmm.set_localutcnanoseconds(g_gnssutc.tv_nsec);
            nmm.set_sourceid(g_srcid);         
            //            if (doDEBUG) { cerr<<humanTimeNow()<<" GPS frame, numwords: "<<(int)payload[4]<<", version: "<<(int)payload[6]<<endl; }
            static int wn, tow;
            auto gpsframe = getGPSFromSFRBXMsg(payload);
            auto cond = getCondensedGPSMessage(gpsframe);
            auto frameno = getbitu(&cond[0], 24+19, 3);
            if(frameno == 1 && sigid==0)
              wn = 2048 + getbitu(&cond[0], 2*24, 10);
            if(!wn) 
              continue; // can't file this yet
              
            tow = 1.5*(getbitu(&cond[0], 24, 17)*4);
            nmm.mutable_gpsi()->set_gnsswn(wn);   // XXX this sucks
            nmm.mutable_gpsi()->set_sigid(sigid);
            nmm.mutable_gpsi()->set_gnsstow(tow); // "with 6 second increments" -- needs to be adjusted
            nmm.mutable_gpsi()->set_gnssid(id.first);
            nmm.mutable_gpsi()->set_gnsssv(id.second);
            nmm.mutable_gpsi()->set_contents(string((char*)gpsframe.c_str(), gpsframe.size()));
            ns.emitNMM( nmm);
            continue;
          }
          if(id.first == 0 && sigid) { // new GPS
            auto cnav = getGPSFromSFRBXMsg(payload);
            static int wn, tow;

            int type = getbitu(&cnav[0], 14, 6);
            tow = 6 * getbitu(&cnav[0], 20, 17) - 12;
            
            if(type == 10) {
              wn = getbitu(&cnav[0], 38, 13);
            }

            if(!wn) 
              continue; // can't file this yet
            
            NavMonMessage nmm;
            nmm.set_type(NavMonMessage::GPSCnavType);
            nmm.set_localutcseconds(g_gnssutc.tv_sec);
            nmm.set_localutcnanoseconds(g_gnssutc.tv_nsec);
            nmm.set_sourceid(g_srcid);         
            nmm.mutable_gpsc()->set_gnsswn(wn);   // XXX this sucks
            nmm.mutable_gpsc()->set_sigid(sigid);
            nmm.mutable_gpsc()->set_gnsstow(tow); // "with 6 second increments" -- needs to be adjusted
            nmm.mutable_gpsc()->set_gnssid(id.first);
            nmm.mutable_gpsc()->set_gnsssv(id.second);
            nmm.mutable_gpsc()->set_contents(string((char*)cnav.c_str(), cnav.size()));
            ns.emitNMM( nmm);            
          }
          else if(id.first ==2) { // GALILEO
            auto inav = getInavFromSFRBXMsg(payload);
            unsigned int wtype = getbitu(&inav[0], 0, 6);

            uint32_t satTOW;
            int msgTOW{0};
            if(getTOWFromInav(inav, &satTOW, &g_galwn)) { // 0, 6, 5
              //            if (doDEBUG) { cerr<<humanTimeNow()<<"    "<<wtype<<" sv "<<id.second<<" tow "<<satTOW << " % 30 = "<< satTOW % 30<<", implied start of cycle: "<<(satTOW - (satTOW %30)) <<endl; }
              msgTOW = satTOW;
              curCycleTOW = satTOW - (satTOW %30);
            }
            else {
              if(curCycleTOW < 0) // did not yet have a start of cycle
                continue;
              //            if (doDEBUG) { cerr<<humanTimeNow()<<"    "<<wtype<<" sv "<<id.second<<" tow "; }
              if(sigid == 5) {
                if(wtype == 2) {
                  msgTOW = curCycleTOW + 20;
                }
                else if(wtype == 4) {
                  msgTOW = curCycleTOW + 22;
                } // next have '6' which sets TOW
                else if(wtype==7 || wtype == 9) {
                  msgTOW = curCycleTOW + 6;
                }
                else if(wtype==8 || wtype == 10) {
                  msgTOW = curCycleTOW + 8;
                }
                else if(wtype==1) {
                  msgTOW = curCycleTOW+30;
                }
                else if(wtype==3) {
                  msgTOW = curCycleTOW + 32; 
                }
                else { // dummy
                  if(id.second != 20) // known broken XXX
                    if (doDEBUG) { cerr<<humanTimeNow()<<" galileo E"<<id.second<<" what kind of wtype is this: "<<wtype<<endl; }
                  continue;
                }
              }
              else {
                if(wtype == 2) {
                  //              if (doDEBUG) { cerr<<humanTimeNow()<<" infered to be 1 "<<curCycleTOW + 31<<endl; }
                  msgTOW = curCycleTOW + 31;
                }
                else if(wtype == 4) {
                  //              if (doDEBUG) { cerr<<humanTimeNow()<<" infered to be 3 "<<curCycleTOW + 33<<endl; }
                  msgTOW = curCycleTOW + 33;
                } // next have '6' which sets TOW
                else if(wtype==7 || wtype == 9) {
                  msgTOW = curCycleTOW + 7;
                }
                else if(wtype==8 || wtype == 10) {
                  msgTOW = curCycleTOW + 9;
                }
                else if(wtype==1) {
                  msgTOW = curCycleTOW + 21;
                }
                else if(wtype==3) {
                  msgTOW = curCycleTOW + 23; 
                }
                else { // dummy
                  if(id.second != 20) // known broken XXX
                    if (doDEBUG) { cerr<<humanTimeNow()<<" galileo E"<<id.second<<" what kind of wtype is this: "<<wtype<<endl; }
                  continue;
                }

              }
            }
            NavMonMessage nmm;
            nmm.set_sourceid(g_srcid);
            nmm.set_type(NavMonMessage::GalileoInavType);
            nmm.set_localutcseconds(g_gnssutc.tv_sec);
            nmm.set_localutcnanoseconds(g_gnssutc.tv_nsec);
            
            nmm.mutable_gi()->set_gnsswn(g_galwn);
            
            nmm.mutable_gi()->set_gnsstow(msgTOW);
            nmm.mutable_gi()->set_gnssid(id.first);
            nmm.mutable_gi()->set_gnsssv(id.second);
            nmm.mutable_gi()->set_sigid(sigid);
            nmm.mutable_gi()->set_contents((const char*)&inav[0], inav.size());
            
            ns.emitNMM( nmm);
          }
          else if(id.first==3) {
            auto gstr = getGlonassFromSFRBXMsg(payload);
            auto cond = getCondensedBeidouMessage(gstr);
            static map<int, BeidouMessage> bms;
            auto& bm = bms[id.second];
            
            uint8_t pageno;
            bm.parse(cond, &pageno);
            
            if(bm.wn < 0) {
              if (doDEBUG) { cerr<<humanTimeNow()<<" BeiDou C"<<id.second<<" WN not yet known, not yet emitting message"<<endl; }
              continue;
            }
            NavMonMessage nmm;
            nmm.set_localutcseconds(g_gnssutc.tv_sec);
            nmm.set_localutcnanoseconds(g_gnssutc.tv_nsec);
            nmm.set_sourceid(g_srcid);
            if(id.second > 5) {
              // this **HARDCODES** that C01,02,03,04,05 emit D2 messages!            
              nmm.set_type(NavMonMessage::BeidouInavTypeD1);
              nmm.mutable_bid1()->set_gnsswn(bm.wn);  // only sent in word 1!!
              nmm.mutable_bid1()->set_gnsstow(bm.sow); 
              nmm.mutable_bid1()->set_gnssid(id.first);
              nmm.mutable_bid1()->set_gnsssv(id.second);
              nmm.mutable_bid1()->set_sigid(sigid);              
              nmm.mutable_bid1()->set_contents(string((char*)gstr.c_str(), gstr.size()));
              ns.emitNMM( nmm);
            }
            else {
              // not sending this: we can't even get the week number right!
              /*
              nmm.set_type(NavMonMessage::BeidouInavTypeD2);
              nmm.mutable_bid2()->set_gnsswn(bm.wn);  
              nmm.mutable_bid2()->set_gnsstow(bm.sow); 
              nmm.mutable_bid2()->set_gnssid(id.first);
              nmm.mutable_bid2()->set_gnsssv(id.second);
              nmm.mutable_bid2()->set_sigid(sigid);              
              nmm.mutable_bid2()->set_contents(string((char*)gstr.c_str(), gstr.size()));
              */
              
            }

            continue;
          }
          else if(id.first==6) {
            //            if (doDEBUG) { cerr<<humanTimeNow()<<" SFRBX from GLONASS "<<id.second<<" @ frequency "<<(int)payload[3]<<", msg of "<<(int)payload[4]<< " words"<<endl; }
            auto gstr = getGlonassFromSFRBXMsg(payload);
            /*
            static map<int, GlonassMessage> gms;
            GlonassMessage& gm = gms[id.second];
            int strno = gm.parse(gstr);
            */
            if(id.second != 255) {
              NavMonMessage nmm;
              nmm.set_localutcseconds(g_gnssutc.tv_sec);  
              nmm.set_localutcnanoseconds(g_gnssutc.tv_nsec);
              nmm.set_sourceid(g_srcid);
              nmm.set_type(NavMonMessage::GlonassInavType);
              nmm.mutable_gloi()->set_freq(payload[3]);
              nmm.mutable_gloi()->set_gnssid(id.first);
              nmm.mutable_gloi()->set_gnsssv(id.second);
              nmm.mutable_gloi()->set_sigid(sigid);              
              nmm.mutable_gloi()->set_contents(string((char*)gstr.c_str(), gstr.size()));
              
              ns.emitNMM( nmm);
            }
          }
          else if(id.first == 1) {// SBAS
            /*            if (doDEBUG) {
              cerr<<humanTimeNow()<<" SBAS "<<id.second<<" frame, numwords: "<<(int)payload[4]<<", version: "<<(int)payload[6]<<", ";
            }
            */

            auto sbas = getSBASFromSFRBXMsg(payload);

            NavMonMessage nmm;
            nmm.set_localutcseconds(g_gnssutc.tv_sec);  
            nmm.set_localutcnanoseconds(g_gnssutc.tv_nsec);
            nmm.set_sourceid(g_srcid);
            nmm.set_type(NavMonMessage::SBASMessageType);
            nmm.mutable_sbm()->set_gnssid(id.first);
            nmm.mutable_sbm()->set_gnsssv(id.second);
            nmm.mutable_sbm()->set_contents(string((char*)sbas.c_str(), sbas.size()));
            
            ns.emitNMM( nmm);
          }
          else
            ; //            if (doDEBUG) { cerr<<humanTimeNow()<<" SFRBX from unsupported GNSSID/sigid combination "<<id.first<<", sv "<<id.second<<", sigid "<<sigid<<", "<<payload.size()<<" bytes"<<endl; }
       
        }
        catch(CRCMismatch& cm) {
          if (doDEBUG) { cerr<<humanTimeNow()<<" Had CRC mismatch!"<<endl; }
        }

      }
      else if(msg.getClass() == 1 && msg.getType() == 0x35) { // UBX-NAV-SAT
        if(version9) // we have UBX-NAV-SIG
          continue;
        //        if (doDEBUG) { cerr<<humanTimeNow()<<" Info for "<<(int) payload[5]<<" svs: \n"; }
        for(unsigned int n = 0 ; n < payload[5]; ++n) {
          int gnssid = payload[8+12*n];
          int sv = payload[9+12*n];

          auto el = (int)(char)payload[11+12*n];
          auto azi = ((int)payload[13+12*n]*256 + payload[12+12*n]);
          auto db = (int)payload[10+12*n];
          //          if (doDEBUG) { cerr<<"gnssid "<<gnssid<<" sv "<<sv<<" el "<<el<<endl; }
          NavMonMessage nmm;
          nmm.set_sourceid(g_srcid);
          nmm.set_localutcseconds(g_gnssutc.tv_sec);
          nmm.set_localutcnanoseconds(g_gnssutc.tv_nsec);
          
          nmm.set_type(NavMonMessage::ReceptionDataType);
          nmm.mutable_rd()->set_gnssid(gnssid);
          nmm.mutable_rd()->set_gnsssv(sv);
          nmm.mutable_rd()->set_db(db);
          nmm.mutable_rd()->set_el(el);
          nmm.mutable_rd()->set_azi(azi);
          nmm.mutable_rd()->set_prres(*((int16_t*)(payload.c_str()+ 14 +12*n)) *0.1);

          uint32_t status;
          memcpy(&status, &payload[16+12*n], 4);
          nmm.mutable_rd()->set_qi(status & 7);
          nmm.mutable_rd()->set_used(status & 8);

          /*
          if (doDEBUG) {
            cerr<<humanTimeNow()<<" "<<gnssid<<","<<sv<<":";
            cerr<<" used " << ((status & 8) == 8);
            cerr<< " qualityind "<<(status & 7);
            cerr<<" db "<<db<<" el "<< el;
            cerr<<" health " << (status & (1<<5));
            cerr<<" sbasCorr " << (status & (1<<16));
            cerr<<" prRes "<<nmm.rd().prres();
            cerr<<" orbsrc " << ((status >> 8)&7);
            cerr<<" eph-avail " << !!(status & (1<<11));
            cerr<<" alm-avail " << !!(status & (1<<12)); 
            cerr<<endl;
          }
          */
          ns.emitNMM( nmm);
        }
      }
      else if(msg.getClass() == 1 && msg.getType() == 0x43) { // UBX-NAV-SIG
        for(unsigned int n = 0 ; n < payload[5]; ++n) {
          int gnssid = payload[8+16*n];
          int sv = payload[9+16*n];
          int qi = payload[15+16*n];
          uint16_t sigflags;
          memcpy(&sigflags, &payload[18+16*n], 2);
          int sigid = 0;

          if(version9) { // we only use this on version9 right now tho
            sigid = payload[10+16*n];
            if(gnssid == 2 && sigid ==6)  // they separate out I and Q, but the rest of UBX doesn't
              sigid = 5;                  // so map it back
            if(gnssid == 2 && sigid ==0)  
              sigid = 1;
            if(gnssid ==0) {
              if(sigid==3)  // L2C is sent as '4' and '3', but the '4' numbers here are bogus
                sigid=4;    // if we see 3,  use it, and change number to 4 to be consistent
              else if(sigid != 0) // sigid 0 = 0
                continue;   // ignore the rest
            }
          }
          else if(gnssid==2) { // version 8 defaults galileo to E1B
            sigid = 1;
          }

          auto db = (int)payload[14+16*n];
          //          if (doDEBUG) { cerr<<"gnssid "<<gnssid<<" sv "<<sv<<" el "<<el<<endl; }
          NavMonMessage nmm;
          nmm.set_sourceid(g_srcid);
          nmm.set_localutcseconds(g_gnssutc.tv_sec);
          nmm.set_localutcnanoseconds(g_gnssutc.tv_nsec);
          
          nmm.set_type(NavMonMessage::ReceptionDataType);
          nmm.mutable_rd()->set_gnssid(gnssid);
          nmm.mutable_rd()->set_gnsssv(sv);
          nmm.mutable_rd()->set_db(db);
          nmm.mutable_rd()->set_prres(*((int16_t*)(payload.c_str()+ 12 +16*n)) *0.1); // ENDIANISM
          nmm.mutable_rd()->set_sigid(sigid);
          nmm.mutable_rd()->set_el(0);
          nmm.mutable_rd()->set_azi(0);
          nmm.mutable_rd()->set_qi(qi);
          nmm.mutable_rd()->set_used(sigflags & 8);
          
          
          ns.emitNMM( nmm);
        }
      }
      else if(msg.getClass() == 1 && msg.getType() == 0x30) { // UBX-NAV-SVINFO
        
      }
      else if(msg.getClass() == 1 && msg.getType() == 0x22) { // UBX-NAV-CLOCK
        struct NavClock
        {
          uint32_t iTowMS;
          int32_t clkBNS;
          int32_t clkDNS;
          uint32_t tAcc;
          uint32_t fAcc;
        } nc;
        memcpy(&nc, &payload[0], sizeof(nc));
//        cerr<<"Clock offset "<< nc.clkBNS<<" nanoseconds, drift "<< nc.clkDNS<<" nanoseconds/second, accuracy " << nc.tAcc<<" ns, frequency accuracy "<<nc.fAcc << " ps/s"<<endl;
  //      cerr<<"hwversion "<<hwversion<<" swversion "<< swversion <<" mods "<< mods <<" serialno "<<serialno<<endl;


        NavMonMessage nmm;
        
        nmm.set_sourceid(g_srcid);
        nmm.set_localutcseconds(g_gnssutc.tv_sec);
        nmm.set_localutcnanoseconds(g_gnssutc.tv_nsec);
        
        nmm.set_type(NavMonMessage::ObserverDetailsType);
        nmm.mutable_od()->set_vendor("Ublox");
        nmm.mutable_od()->set_hwversion(hwversion);
        nmm.mutable_od()->set_swversion(swversion);
        nmm.mutable_od()->set_serialno(serialno);
        nmm.mutable_od()->set_modules(mods);
        nmm.mutable_od()->set_clockoffsetns(nc.clkBNS);
        nmm.mutable_od()->set_clockoffsetdriftns(nc.clkDNS);
        nmm.mutable_od()->set_clockaccuracyns(nc.tAcc);
        nmm.mutable_od()->set_freqaccuracyps(nc.fAcc);

        nmm.mutable_od()->set_owner(owner);
        nmm.mutable_od()->set_remark(remark);
        nmm.mutable_od()->set_recvgithash(g_gitHash);
        nmm.mutable_od()->set_uptime(time(0) - starttime);
        
        
        ns.emitNMM( nmm);
      }
      else if(msg.getClass() == 0x02 && msg.getType() == 0x14) { // UBX-RXM-MEASX

        //        if (doDEBUG) { cerr<<humanTimeNow()<<" Got RXM-MEASX for "<<(int)payload[34]<<" satellites, r0 "<< (int)payload[30]<<" r1 " <<(int)payload[31]<<endl; }
        for(unsigned int n = 0 ; n < payload[34] ; ++n) {
          uint16_t wholeChips;
          uint16_t fracChips;
          //          uint8_t intCodePhase = payload[64+24*n];
          uint32_t codePhase;

          memcpy(&wholeChips, &payload[56+24*n], 2);
          memcpy(&fracChips, &payload[58+24*n], 2);
          memcpy(&codePhase, &payload[60+24*n], 4);
          
          //          if (doDEBUG) { cerr<<humanTimeNow()<<" "<<(int)payload[44+24*n]<<","<<(int)payload[45+24*n]<<" whole-chips "<<wholeChips<<" frac-chips "<<fracChips<<" int-code-phase " <<(int)intCodePhase <<" frac-code-phase "<<ldexp(codePhase, -21) << " mpath " << (int)payload[47+24*n] << " r1 " << (int)payload[66+24*n] << " r2 " <<(int)payload[67+24*n]<<endl; }
        }
      }
      else if(msg.getClass() == 0x02 && msg.getType() == 0x59) { // UBX-RXM-RLM

        NavMonMessage nmm;
        nmm.set_sourceid(g_srcid);
        nmm.set_localutcseconds(g_gnssutc.tv_sec);
        nmm.set_localutcnanoseconds(g_gnssutc.tv_nsec);
        
        nmm.set_type(NavMonMessage::SARResponseType);

        // short version:
        //   0         1   2    3      4   -  11    12     13,14 15
        // version, type, sv, reserved beacon id  msg-code param res2

        // long version:
        //   0         1   2    3      4   -  11    12     13-24  25
        // version, type, sv, reserved beacon id  msg-code params res2
        
        nmm.mutable_sr()->set_gnssid(2); // Galileo only for now
        nmm.mutable_sr()->set_gnsssv(payload[2]);
        nmm.mutable_sr()->set_sigid(1); // 
        nmm.mutable_sr()->set_type(payload[1]);
        nmm.mutable_sr()->set_identifier(string((char*)payload.c_str()+4, 8));
        nmm.mutable_sr()->set_code(payload[12]);
        nmm.mutable_sr()->set_params(string((char*)payload.c_str()+13, payload.size()-14));

        
        string hexstring;
        for(int n = 0; n < 15; ++n)                                   
          hexstring+=fmt::sprintf("%x", (int)getbitu(payload.c_str(), 36 + 4*n, 4));

        ns.emitNMM(nmm);
      }
      else if(msg.getClass()==39 && msg.getType()==0) {
        NavMonMessage nmm;
        nmm.set_sourceid(g_srcid);
        nmm.set_localutcseconds(g_gnssutc.tv_sec);
        nmm.set_localutcnanoseconds(g_gnssutc.tv_nsec);
        
        nmm.set_type(NavMonMessage::DebuggingType);
        nmm.mutable_dm()->set_type(0); 
        nmm.mutable_dm()->set_payload(string((char*)&payload[0], payload.size())); 
        ns.emitNMM( nmm);
      }
      else if(msg.getClass() == 0x01 && msg.getType() == 0x3B) { // UBX-NAV-SVIN
        struct NavSin
        {
          uint8_t ver;
          uint8_t res[3];
          uint32_t iTow;
          uint32_t dur;
          int32_t meanXCM, meanYCM, meanZCM;
          int8_t meanXHP, meanYHP, meanZHP;
          uint8_t res2;
          int32_t meanAcc;
          int32_t obs;
          int8_t valid;
          int8_t active;
          uint8_t res3[2];
        } __attribute__((packed));
        NavSin NS;
        static NavSin lastNS;
        
        if(payload.size() != sizeof(NS)) {
          cerr<<"Wrong NAV-SVIN message size, skipping"<<endl;
          continue;
        }
        memcpy(&NS, payload.c_str(), sizeof(NS));
        NS.res[0] = NS.res[1] = NS.res[2] = 0;
        NS.res2=0;
        NS.res3[0] = NS.res3[1] = 0;
        NS.iTow = 0;
        if(memcmp(&NS, &lastNS, sizeof(NS))) {
          cerr<<humanTimeNow()<<" NAV-SVIN ver "<<(int)NS.ver<< " valid "<< (int)NS.valid<<" active " << (int)NS.active<<" duration "<<NS.dur<<"s meanAcc " <<NS.meanAcc /100<< "cm obs "<<NS.obs<<" ";
          cerr<<std::fixed<<" ("<<NS.meanXCM + 0.01*NS.meanXHP<<", "<<NS.meanYCM  + 0.01*NS.meanYHP<<", "<<NS.meanZCM  + 0.01*NS.meanZHP<<")";
          
          auto latlonh = ecefToWGS84Deg((NS.meanXCM +0.01*NS.meanXHP)/100.0, 
                                        (NS.meanYCM +0.01*NS.meanYHP)/100.0, 
                                        (NS.meanZCM +0.01*NS.meanZHP)/100.0);
          cerr<<" lat "<< get<0>(latlonh)<<" lon "<< get<1>(latlonh) << " h " << get<2>(latlonh) << endl;
        }
        lastNS = NS;
      }      
      else if(msg.getClass() == 0x0d && msg.getType() == 0x04) { // UBX-TIM-SVIN
        struct TimSin
        {
          uint32_t dur;
          int32_t meanXCM, meanYCM, meanZCM;
          uint32_t meanVar;
          uint32_t obs;
          int8_t valid;
          int8_t active;
          uint8_t res3[2];
        } __attribute__((packed));
        TimSin TS;
        static TimSin lastTS;
        
        if(payload.size() != sizeof(TS)) {
          cerr<<"Wrong NAV-SVIN message size, skipping"<<endl;
          continue;
        }
        memcpy(&TS, payload.c_str(), sizeof(TS));
        TS.res3[0] = TS.res3[1] = 0;
        if(memcmp(&TS, &lastTS, sizeof(TS))) {
          cerr<<humanTimeNow()<<" TIM-SVIN valid "<< (int)TS.valid<<" active " << (int)TS.active<<" duration "<<TS.dur<<"s meanAcc " <<sqrt(TS.meanVar)/10<< "cm obs "<<TS.obs<<" ";
          cerr<<std::fixed<<"("<<TS.meanXCM <<", "<<TS.meanYCM <<", "<<TS.meanZCM<<")"<<endl;
        }
        lastTS = TS;
      }
      else if(msg.getClass() == 0x0a && msg.getType() == 0x09) { // UBX-MON-HW
        struct MonHW {
          uint32_t pinSel, pinBank, pinDir, pinVal;

          uint16_t noisePerMS;
          uint16_t agcCnt;

          uint8_t aStatus;
          uint8_t aPower;
          uint8_t flags;
          uint8_t res1;
          
          uint32_t usedMask;
          
          uint8_t VP[17];
          uint8_t jamInd;
          uint16_t res2;
          uint32_t pinIrq, pullH, pullL;
        } __attribute__((packed));
        MonHW mhw;
        memcpy(&mhw, payload.c_str(), sizeof(MonHW));
        //        cerr << "agcCnt "<< mhw.agcCnt <<" jamind " << (unsigned int) mhw.jamInd <<" flags "<< (unsigned int)mhw.flags << endl;
        NavMonMessage nmm;
        nmm.set_sourceid(g_srcid);
        nmm.set_localutcseconds(g_gnssutc.tv_sec);
        nmm.set_localutcnanoseconds(g_gnssutc.tv_nsec);
        
        nmm.set_type(NavMonMessage::UbloxJammingStatsType);
        nmm.mutable_ujs()->set_noiseperms(mhw.noisePerMS);
        nmm.mutable_ujs()->set_agccnt(mhw.agcCnt);
        nmm.mutable_ujs()->set_flags(mhw.flags);
        nmm.mutable_ujs()->set_jamind(mhw.jamInd);
        ns.emitNMM(nmm);
      }
      else if(msg.getClass() == 0x01 && msg.getType() == 0x25) { // UBX-NAV-TIMEGAL
        memcpy(&tstate.gal, &payload[0], sizeof(TIMEGAL));
        //        cerr << "TIMEGAL itow: "<<tstate.gal.itow<<", fGalTow: "<<tstate.gal.fGalTow<<", tAcc: "<<tstate.gal.tAcc<< ", valid: "<< !!tstate.gal.valid<< endl;
        tstate.transmitIfComplete(ns);
      }
      else
      if(msg.getClass() == 0x01 && msg.getType() == 0x24) { // UBX-NAV-TIMEBDS
        memcpy(&tstate.bds, &payload[0], sizeof(TIMEBDS));
//        cerr << "TIMEBDS itow: "<<tstate.bds.itow<<", fSow: "<<tstate.bds.fSow<<", tAcc: "<<tstate.bds.tAcc<< ", valid: "<< !!tstate.bds.valid  << endl;
        tstate.transmitIfComplete(ns);
      }
      else
      if(msg.getClass() == 0x01 && msg.getType() == 0x23) { // UBX-NAV-TIMEGLO
        memcpy(&tstate.glo, &payload[0], sizeof(TIMEGLO));
//        cerr << "TIMEGLO itow: "<<tstate.glo.itow<<", fTod: "<<tstate.glo.fTod<<", tAcc: "<<tstate.glo.tAcc<< ", valid: "<<!!tstate.glo.valid<<endl;
        tstate.transmitIfComplete(ns);
      }
      else
      if(msg.getClass() == 0x01 && msg.getType() == 0x20) { // UBX-NAV-TIMEGPS
        memcpy(&tstate.gps, &payload[0], sizeof(TIMEGPS));
        //        cerr << "TIMEGPS itow: "<<tstate.gps.itow<<", ftow: "<<tstate.gps.ftow<<", tAcc: "<<tstate.gps.tAcc<< ", valid: "<< !!tstate.gps.valid<<endl;
        tstate.transmitIfComplete(ns);
      }
      else 
        if (doDEBUG) { cerr<<humanTimeNow()<<" Unknown UBX message of class "<<(int) msg.getClass() <<" and type "<< (int) msg.getType()<< " of "<<payload.size()<<" bytes"<<endl; }

      //      writen2(1, payload.d_raw.c_str(),msg.d_raw.size());
    }
    catch(UBXMessage::BadChecksum &e) {
      if (doDEBUG) { cerr<<humanTimeNow()<<" Bad UBX checksum, skipping message"<<endl; }
    }
    catch(EofException& em) {
      break;
    }
  }
  cerr<<"Done after reading "<<lseek(fd, 0, SEEK_CUR)<<" bytes, flushing buffers.."<<endl;
  if(!g_fromFile)
    tcsetattr(fd, TCSANOW, &g_oldtio);                                          
}                                                                         


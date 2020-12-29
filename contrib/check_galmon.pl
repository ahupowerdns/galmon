#!/usr/bin/env perl
#
# Nagios plugin to check the last update of an Galileo Observer as per
# https://galmon.eu/
#
# The plugin used check_json.pl as basis as found at
# https://github.com/c-kr/check_json
#
# To not stress the monitoring platform, please only check every 30 minutes
# by including the following two items in the service definition:
# check_interval 30
# retry_interval 10

use warnings;
use strict;
use HTTP::Request::Common;
use LWP::UserAgent;
use JSON;
use Monitoring::Plugin;
use Data::Dumper;

my $url = "https://galmon.eu/observers.json";

my $np = Monitoring::Plugin->new(
    usage => "Usage: %s -i|--id <station_id> "
    . "[ -u|--url <alterative url for " . $url . "> ] "
    . "[ -s|--serial <receiver serial> ] "
    . "[ -c|--critical <seconds> ] [ -w|--warning <seconds> ] "
    . "[ -t|--timeout <timeout> ] "
    . "[ -T|--contenttype <content-type> ] "
    . "[ --ignoressl ] "
    . "[ -h|--help ] ",
    version => '0.1',
    blurb   => 'Nagios plugin to check Galmon observer last-seen freshness',
    extra   => "\nExample: \n"
    . "check_galmon.pl --id 100 -w 900 -c 1800",
    plugin  => 'check_galmon',
    timeout => 15,
    shortname => "GalMon:",
);

 # add valid command line options and build them into your usage/help documentation.
$np->add_arg(
    spec => 'id|i=i',
    help => '-i, --id 100',
    required => 1,
);

$np->add_arg(
    spec => 'warning|w=s',
    help => '-w, --warning INTEGER:INTEGER. See '
    . 'http://nagiosplug.sourceforge.net/developer-guidelines.html#THRESHOLDFORMAT '
    . 'for the threshold format. '
);

$np->add_arg(
    spec => 'critical|c=s',
    help => '-c, --critical INTEGER:INTEGER. See '
    . 'http://nagiosplug.sourceforge.net/developer-guidelines.html#THRESHOLDFORMAT '
    . 'for the threshold format. '
);

$np->add_arg(
    spec => 'url|u=s',
    default => $url,
    help => "-u, --url <alterative url for " . $url . ">"
);

$np->add_arg(
    spec => 'serial|s=s',
    default => undef,
    help => '-s, --serial <receiver serial> '
    . 'as a safety check, verify the serial of the received reported.'

);

$np->add_arg(
    spec => 'contenttype|T=s',
    default => 'application/json',
    help => '-T, --contenttype application/json '
    . 'Content-type accepted if different from application/json'
);

$np->add_arg(
    spec => 'ignoressl',
    help => '--ignoressl Ignore bad ssl certificates'
);

## Parse @ARGV and process standard arguments (e.g. usage, help, version)
$np->getopts;
if ($np->opts->verbose) { (print Dumper ($np))};

## GET URL
my $ua = LWP::UserAgent->new;

$ua->env_proxy;
$ua->agent("check_galmon/0.1");
$ua->default_header('Accept' => 'application/json');
$ua->protocols_allowed( [ 'http', 'https'] );
$ua->parse_head(0);
$ua->timeout($np->opts->timeout);

if ($np->opts->ignoressl) {
    $ua->ssl_opts(verify_hostname => 0, SSL_verify_mode => 0x00);
}

if ($np->opts->verbose) { (print Dumper ($ua))};

my $response = $ua->request(GET $np->opts->url);

if ($response->is_success) {
    if (!($response->header("content-type") =~ $np->opts->contenttype)) {
        $np->nagios_exit(UNKNOWN,"UNKNOWN: Content type is not JSON: ".$response->header("content-type"));
    }
} else {
    $np->nagios_exit(CRITICAL, "CRITICAL: Connection failed: ".$response->status_line);
}

## Parse JSON
my $json_response = decode_json($response->content);
if ($np->opts->verbose) { 
	print "Dumping JSON response.\n";
	print Dumper ($json_response);
	print "End of JSON response dump.\n"
};

my $epoch=time();
foreach my $tmpObj (@$json_response){
	if ( $tmpObj->{"id"} == $np->opts->id ){
		my $serialno = $tmpObj->{"serialno"};
		my $lastseen = $tmpObj->{"last-seen"};

		my $deltatime = time() - $lastseen;
		# Threshold check
		my $retcode = $np->check_threshold(
			check => $deltatime,
			warning => $np->opts->warning,
			critical => $np->opts->critical,
		);

		# Check serial
		my $serialmsg = "";
		if ( $np->opts->serial ) {
			if ( $serialno eq $np->opts->serial ) {
				$serialmsg = " - Serial: " . $serialno . " matched";
			} else {
				$serialmsg = " - Serial: " . $serialno . " NOT matched";
				$retcode=CRITICAL;
			}
		}

		$np->plugin_exit( $retcode, "Last update received " . $deltatime . " secs ago" . $serialmsg . ".");
	}
}


# If we end up here, the ID was never fouond
$np->nagios_exit(UNKNOWN,"UNKNOWN: Observer ID: \"" . $np->opts->id . "\" not found.\n");


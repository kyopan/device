#!/bin/env perl
use strict;
use warnings;
use Furl;
use Time::HiRes qw/gettimeofday tv_interval/;

my $base               = "http://192.168.2.2";
my $key                = "bd4d0211-9312-44ee-8b0a-02ad8682fad7";
my $number_of_requests = 10;

my $agent = Furl->new(
    agent   => 'Bench/1.0',
    timeout => 20,
);

my $get_message = sub {
    print "GET /messages\n";
    my $res = $agent->get( "$base/messages" );
    die $res->status_line unless $res->is_success;
};

my $post_message = sub {
    # エアコンオン
    print "POST /messages\n";
    my $res = $agent->post( "$base/messages", [],
                            'message={"freq":38,"format":"raw","data":[6573,3257,867,824,866,825,865,2463,866,826,864,2462,867,825,864,827,863,828,863,2464,866,2462,867,826,864,826,865,826,864,2463,866,2461,868,825,865,826,864,827,864,826,865,826,864,826,865,826,864,826,865,825,866,826,864,826,865,826,864,827,864,2462,867,826,864,826,865,826,864,827,864,827,864,826,864,826,865,2461,868,825,865,826,865,826,864,827,864,2462,867,2461,867,2461,868,2461,867,2461,868,2461,867,2461,868,2461,867,826,864,826,865,2461,866,826,864,827,864,828,863,827,864,826,865,826,865,825,865,826,865,2462,867,2462,866,825,865,826,865,2462,867,825,865,826,865,825,865,826,865,2461,868,825,865,2462,867,2462,867,825,865,826,864,827,864,825,865,826,865,826,865,825,865,826,865,826,864,827,864,826,865,826,864,826,865,2462,867,825,865,827,864,825,864,827,863,827,863,829,863,827,864,827,864,826,865,826,864,826,865,826,865,825,865,827,864,827,864,826,864,827,864,826,864,826,865,826,865,826,864,826,865,826,864,827,864,827,864,826,864,827,864,826,865,2462,867,825,865,2462,867,825,865,826,864,826,865,2463,866,2461,867,826,865,825,865,827,864,2462,867,2461,867]}',
                        );
    die $res->status_line unless $res->is_success;
};

my $post_keys = sub {
    print "POST /keys\n";
    my $res = $agent->post( "$base/keys", [], [] );
    die $res->status_line unless $res->is_success;
};

my @requests = (
    $get_message,
    $post_message,
    $post_keys
);

for my $i (1..$number_of_requests) {
    print "[$i] ";
    my $time = [gettimeofday];
    $requests[ int(rand scalar @requests) ]();
    # $requests[ 2 ]();
    my $elapsed = tv_interval( $time, [gettimeofday] );
    printf( " %.2f[s]\n", $elapsed );
}
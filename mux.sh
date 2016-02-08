#!/bin/sh

# usage: mux.sh <inputfile.mp4>

# assumes h264/aac encoding on input file. for other codecs
# the -bsf:v h264_mp4toannexb
# and -mpegts_service_type advanced_codec_digital_hdtv
# may need to be removed or replaced accordingly.

network_name="MyNetwork"
channel_name="MyChannel"
usb_device=0
dvb_channel=60
network_id=0x0700
stream_id=0x0001
service_id=800

MODULATION="-d $usb_device -c $dvb_channel --gain -25 --guard-interval 1/4 --code-rate 1/2"
MUXRATE="$(sendts-it9507 --muxrate $MODULATION)"

ffmpeg \
	-i "$1" \
	-f mpegts \
	-c:v copy -bsf:v h264_mp4toannexb \
	-c:a copy \
	-mpegts_original_network_id "$network_id" \
	-mpegts_transport_stream_id "$stream_id" \
	-mpegts_service_type advanced_codec_digital_hdtv \
	-mpegts_service_id "$service_id" \
	-mpegts_pmt_start_pid 0x30 \
	-mpegts_copyts 1 \
	-muxrate $((MUXRATE-1000)) \
	-streamid 0:0x31 \
	-streamid 1:0x32 \
	-metadata service_provider="$network_name" \
	-metadata service_name="$channel_name" \
	-fflags +sortdts \
	- | \
sendts-it9507 $MODULATION


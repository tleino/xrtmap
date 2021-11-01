#!/bin/sh

IFACE=em0
FILTER='tcp[tcpflags] & (tcp-syn|tcp-ack) == tcp-syn|tcp-ack'

GEOIPDB=/var/db/GeoIP/GeoLite2-City.mmdb

find_location() {
	(mmdblookup --file $GEOIPDB --ip $1 location \
		| sed 's/<.*>//g' \
		| grep -A1 $2 \
		| grep -v $2 \
		| awk '{ printf("%f", $1) }' \
	) 2>/dev/null
}

print_location() {
	while read ; do
		LAT=$(find_location $REPLY latitude)
		LON=$(find_location $REPLY longitude)
		if [ "${LAT}" != "" ] ; then
			echo $LAT $LON
		fi
	done
}

get_ips() {
	tcpdump -l -q -n -i $IFACE $FILTER \
		| awk '{ print $2; fflush(stdout); }' \
		| awk 'BEGIN { FS="." } \
		{ printf("%d.%d.%d.%d\n", $1, $2, $3, $4); fflush(stdout); }'
}

get_ips | print_location

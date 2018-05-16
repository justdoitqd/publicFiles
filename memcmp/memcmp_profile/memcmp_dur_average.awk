#!/usr/bin/awk -f
BEGIN {sum = 0.0; num = 0;}

/memcmp/ {
	num = num + 1
	sum = sum + $2
}

END {
	avg = sum/num
	print "average us = " avg " us"
}


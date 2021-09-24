len_total=0
for f in *.opus; do
	len_total=$(( 10#$len_total + 10#$(ffprobe "$f" 2>&1 | grep DURATION | sed -rn "s/.*: (.*)/\1/p" | ./time_to_s.sh)))
	echo $len_total
done
echo "The total length is $len_total seconds"

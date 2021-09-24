read input_time

IFS=':' read -r -a array <<< "$input_time"

hour="${array[0]}"
min="${array[1]}"
sec="${array[2]}"
IFS='.' read -r -a sec <<< "$sec"
sec="${sec[0]}"

echo $((10#$hour*3600 + 10#$min*60 + 10#$sec))

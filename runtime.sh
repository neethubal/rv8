start=$(date +'%H:%M:%S.%N')
StartDate=$(date -u -d "$start" +"%s.%N")
echo $start
echo $StartDate
sleep 2
sleep 0.5
sleep 0.001
end=$(date +'%H:%M:%S.%N')
EndDate=$(date -u -d "$end" +"%s.%N")
echo $end
echo $EndDate

date -u -d "0 $FinalDate sec - $StartDate sec" +"%S.%N"


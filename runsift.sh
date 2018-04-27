FILES=build/riscv64-unknown-elf/bin
for f in `ls $FILES`
do
    if [ $f == "test-infinite-loop" ]
    then
        continue
    fi 
    echo $f
    # START=$(date +%s);
    start=$(date +'%H:%M:%S.%N')
    StartDate=$(date -u -d "$start" +"%s.%N")
    rv-jit -sift build/riscv64-unknown-elf/bin/$f > output/out/$f
    mv rv8.sift output/sift/$f.sift
    # END=$(date +%s);
    end=$(date +'%H:%M:%S.%N')
    EndDate=$(date -u -d "$end" +"%s.%N")
    
    echo -n $f ' : ' `grep -c INSTR output/out/$f` ' : ' >> output/summary
    # echo $((END-START)) >> output/summary
    date -u -d "0 $EndDate sec - $StartDate sec" +"%S.%N" >> output/summary
done

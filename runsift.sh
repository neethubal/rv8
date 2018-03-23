FILES=build/riscv64-unknown-elf/bin
for f in `ls $FILES`
do
    echo $f
    if [ $f == "test-infinite-loop" ]
    then
        continue
    fi 
    START=$(date +%s);
    rv-jit -sift build/riscv64-unknown-elf/bin/$f > output/out/$f
    mv rv8.sift output/sift/rv8_$f.sift
    END=$(date +%s);
    
    echo -n $f ' : ' `grep -c INSTR output/out/$f` ' : ' >> output/summary
    echo $((END-START)) | awk '{print int($1/60)":"int($1%60)}' >> output/summary
done
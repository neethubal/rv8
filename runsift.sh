FILES=build/riscv64-unknown-elf/bin
for f in `ls $FILES`
do
    echo $f

    START=$(date +%s);
    rv-jit -sift build/riscv64-unknown-elf/bin/$f > output/tmp1_$f
    mv rv8.sift output/rv8_$f.sift
    END=$(date +%s);
    
    echo -n $f ' : ' `grep -c INSTR output/tmp1_$f` ' : ' >> output/summary
    echo $((END-START)) | awk '{print int($1/60)":"int($1%60)}' >> output/summary
done
cases=("1K" "2K" "4K" "5K" "10K" "128K" "129K" "4M" "8M" "9M" "16M" "65M" "127M" "257M")
poolname="data"

for i in ${cases[@]}
do
	echo "test size :$i"
	dd if=/dev/urandom of=file bs=$i count=1 > /dev/null 2>&1
	./striprados -p$poolname -u$i file
	if [[ $? -ne 0 ]] ;then
		echo "upload wrong"
	fi

	./striprados -p$poolname -g$i file.out
	if [[ $? -ne 0 ]] ;then
		echo "download wrong"
	fi
	md1=`md5sum file|awk '{print $1}'`
	md2=`md5sum file.out|awk '{print $1}'`
	if [[ $md1 != $md2 ]] ;then
		echo "wrong"
		exit
	fi
	./striprados -p$poolname -r$i
	rm -rf file; rm -rf file.out
done

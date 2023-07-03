# Call as
#   ./test.sh path-to-firmware
# to test the emulator. The firmware has to be the one with md5 checksum
#   823609edf7fd72863ca37e7e4868cb3d
set -e

firmware=$1
dumpfile=/tmp/lapocket-dump.tmp
errfile=/tmp/lapocket-err.tmp
cardfile=/tmp/lapocket-card.tmp

cleanup () {
	rm -f $dumpfile $errfile $cardfile
}
trap cleanup EXIT

fail () {
	echo "FAILURE"
	mv $errfile ../failure.out
	exit 1
}

touch "$cardfile"
truncate -s 256M "$cardfile"

cd ./tests
for filename in *.out; do
	echo -n "${filename%.out}... "
	../lapocket -f ${filename%.out} -C $cardfile $firmware > $dumpfile 2>$errfile || fail
	diff $filename $dumpfile > $errfile 2>&1 || fail
	echo "SUCCESS"
done

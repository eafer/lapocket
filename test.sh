# Call as
#   ./test.sh path-to-firmware
# to test the emulator. The firmware has to be the one with md5 checksum
#   823609edf7fd72863ca37e7e4868cb3d
set -e

usage () {
	echo "usage: ./test.sh path-to-firmware"
	exit 1
}

[ "x$0" = "x./test.sh" ] || usage

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

rm -rf ./tests/bin
cd ./tests
mkdir bin
for filename in *.out; do
	echo -n "${filename%.out}... "
	# TODO: fully support the card in all tests
	if [ "$filename" = "0027.out" ]; then
		../lapocket --headless -f ${filename%.out} -C $cardfile $firmware > $dumpfile 2>$errfile || fail
	else
		../lapocket --headless -f ${filename%.out} $firmware > $dumpfile 2>$errfile || fail
	fi
	diff $filename $dumpfile > $errfile 2>&1 || fail
	echo "SUCCESS"
done

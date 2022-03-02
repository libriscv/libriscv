TMPFILE="/tmp/test.log"
./debug_rvlinux $1 > $TMPFILE | less +F $TMPFILE

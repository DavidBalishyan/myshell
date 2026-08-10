printf 'zero=%s count=%s\n' "$0" "$#"
for x in "$@"; do
    printf '<%s>\n' "$x"
done
IFS=:
printf 'star=<%s>\n' "$*"
set -- red "green blue"
shift
printf 'shift=<%s>\n' "$1"

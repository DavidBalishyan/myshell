unset x
printf '%s\n' "${x-word}:${x:-word}"
x=
printf '%s\n' "${x-word}:${x:-word}"
unset x
printf '%s\n' "${x:=assigned}:$x"
printf '%s\n' "${x+plus}:${x:+colon-plus}"
x='a b'
printf '<%s>\n' $x
printf '<%s>\n' "$x"
IFS=:
x='one::three:'
printf '[%s]\n' $x
echo "$((2 + 3 * 4))"
word=hello
echo "${#word}"
IFS=
set -- one two
printf 'joined=<%s>\n' "$*"
printf 'brace-at=<%s>\n' "${@}"
x=2
echo "$((x *= 5)):$x:$((x > 3 ? 7 : 9))"

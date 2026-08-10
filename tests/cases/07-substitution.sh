x=$(printf 'one\n\n')
printf '<%s>\n' "$x"
y=`printf 'two\n'`
printf '<%s>\n' "$y"
printf '<%s>\n' "$(printf '%s' "$(echo nested)")"

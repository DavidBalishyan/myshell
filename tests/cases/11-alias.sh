alias hi='echo hello'
hi world
alias formatted='printf "alias=<%s>\n"'
formatted yes
unalias hi
hi 2>/dev/null || echo removed

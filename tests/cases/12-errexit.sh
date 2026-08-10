set -e
false && echo wrong
true || echo wrong
if false; then echo wrong; fi
(false; echo wrong) | cat
echo survived-contexts
false

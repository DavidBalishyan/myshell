printf 'foo\nbar\n' | grep bar | tr a-z A-Z
false | true
echo "last=$?"
true | false
echo "last=$?"
sleep 0.01 &
pid=$!
wait "$pid"
echo "wait=$?"

if false; then
    echo no
elif true; then
    echo yes
else
    echo no
fi
i=0
while [ "$i" -lt 3 ]; do
    echo "w$i"
    i=$((i + 1))
done
until [ "$i" -eq 5 ]; do
    i=$((i + 1))
done
echo "u$i"
for x in a b c; do
    echo "f$x"
done
case abc in
    a*) echo matched;;
    *) echo missed;;
esac
pattern='b*'
case beta in
    "$pattern") echo wrong;;
    $pattern) echo expanded-pattern;;
esac

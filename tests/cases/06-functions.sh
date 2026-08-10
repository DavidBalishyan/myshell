greet() {
    printf 'hello %s\n' "$1"
    return 7
}
greet world
echo "return=$?"
x=outside
(
    x=inside
    echo "$x"
)
echo "$x"
{
    x=group
    echo "$x"
}
echo "$x"

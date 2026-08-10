eval 'made=value; echo "$made"'
echo "$made"
set -- -a -b value rest
while getopts 'ab:' option; do
    printf '%s:%s:%s\n' "$option" "${OPTARG-}" "$OPTIND"
done
shift "$((OPTIND - 1))"
echo "remaining=$*"
. "$LSH_TEST_ROOT/source.inc"
echo "sourced=$sourced_value"
set -f
echo no-match-*.lsh
set +f
umask 022
umask
FOO=passed command sh -c 'echo "command-env=$FOO"'
command -p true

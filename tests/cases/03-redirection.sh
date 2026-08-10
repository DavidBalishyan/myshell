echo first >file
echo second >>file
cat <file
sh -c 'echo out; echo err >&2' >combined 2>&1
cat combined
sh -c 'echo out; echo err >&2' 2>&1 >separate
cat separate
echo descriptor 3>fdfile >&3
cat fdfile
exec 4>persistent
echo held >&4
cat persistent

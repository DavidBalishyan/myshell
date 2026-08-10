x=world
cat <<EOF
hello $x
EOF
cat <<'EOF'
literal $x
EOF
cat <<A <<B
first
A
second
B

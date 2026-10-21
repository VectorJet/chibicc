#!/bin/bash
# LLVM backend tests.
#
# Usage: test/llvm/run.sh <chibicc built with WITH_LLVM=1>
#
# cases/*.c: must compile with --backend=llvm, link, run, and exit with the
#   status named by a line "// EXPECT-EXIT: N".
# unsupported/*.c: must be rejected at compile time, and the diagnostic must
#   contain the text named by a line "// EXPECT-ERROR: <text>".
# The command-line checks at the end cover option validation.

chibicc=$1
dir=$(cd "$(dirname "$0")" && pwd)
tmp=$(mktemp -d /tmp/chibicc-llvm-XXXXXX)
trap 'rm -rf "$tmp"' EXIT
fail=0

for src in "$dir"/cases/*.c; do
  name=$(basename "$src" .c)
  expect=$(sed -nE 's#^// EXPECT-EXIT: *([0-9]+).*#\1#p' "$src" | head -n 1)
  if [ -z "$expect" ]; then
    echo "testing $name ... failed (no EXPECT-EXIT line)"
    fail=1
    continue
  fi
  # An optional "// LINK: file.c" line names a helper in helpers/ that is built
  # with the host C compiler and linked in as an object.
  extra=()
  helper=$(sed -nE 's#^// LINK: *##p' "$src" | head -n 1)
  if [ -n "$helper" ]; then
    ${CC:-cc} -c "$dir/helpers/$helper" -o "$tmp/$name.helper.o" || {
      echo "testing $name ... failed (helper build)"
      fail=1
      continue
    }
    extra=("$tmp/$name.helper.o")
  fi
  if ! "$chibicc" --backend=llvm -o "$tmp/$name" "$src" "${extra[@]}" 2> "$tmp/$name.err"; then
    echo "testing $name ... failed (compile)"
    cat "$tmp/$name.err"
    fail=1
    continue
  fi
  "$tmp/$name"
  got=$?
  if [ "$got" -eq "$expect" ]; then
    echo "testing $name ... passed"
  else
    echo "testing $name ... failed (exit $got, expected $expect)"
    fail=1
  fi
done

for src in "$dir"/unsupported/*.c; do
  name=$(basename "$src" .c)
  expect=$(sed -nE 's#^// EXPECT-ERROR: *##p' "$src" | head -n 1)
  if "$chibicc" --backend=llvm -o "$tmp/$name" "$src" 2> "$tmp/$name.err"; then
    echo "testing $name ... failed (compiled, expected rejection)"
    fail=1
  elif grep -qF -- "$expect" "$tmp/$name.err"; then
    echo "testing $name ... passed"
  else
    echo "testing $name ... failed (diagnostic missing: $expect)"
    cat "$tmp/$name.err"
    fail=1
  fi
done

# expect_reject NAME SUBSTRING COMMAND...: the command must fail and print SUBSTRING.
expect_reject() {
  local name=$1 want=$2
  shift 2
  if "$@" 2> "$tmp/cli.err"; then
    echo "testing $name ... failed (accepted)"
    fail=1
  elif grep -qF -- "$want" "$tmp/cli.err"; then
    echo "testing $name ... passed"
  else
    echo "testing $name ... failed"
    cat "$tmp/cli.err"
    fail=1
  fi
}

host=$(uname -m)
other=aarch64-unknown-linux-gnu
[ "$host" = aarch64 ] && other=x86_64-unknown-linux-gnu

expect_reject "reject unknown --backend value" "unknown backend" "$chibicc" --backend=bogus -c -o "$tmp/x.o" "$dir/cases/return_const.c"
expect_reject "reject -S with --backend=llvm" "not supported with --backend=llvm" "$chibicc" --backend=llvm -S -o "$tmp/x.s" "$dir/cases/return_const.c"
expect_reject "reject --target without --backend=llvm" "requires --backend=llvm" "$chibicc" --target=$other -c -o "$tmp/x.o" "$dir/cases/return_const.c"
expect_reject "reject unknown --target" "unknown target" "$chibicc" --backend=llvm --target=nonsense-triple -c -o "$tmp/x.o" "$dir/cases/return_const.c"
expect_reject "reject known but disabled --target" "not enabled yet" "$chibicc" --backend=llvm --target=$other -c -o "$tmp/x.o" "$dir/cases/return_const.c"

if "$chibicc" --backend=llvm -c -o "$tmp/obj.o" "$dir/cases/return_const.c" && [ -f "$tmp/obj.o" ]; then
  echo "testing -c emits an object ... passed"
else
  echo "testing -c emits an object ... failed"
  fail=1
fi

exit $fail

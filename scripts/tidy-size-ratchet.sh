#!/usr/bin/env bash
# scripts/tidy-size-ratchet.sh: judges clang-tidy's size and complexity
# lines against scripts/tidy-size-baseline.txt, for scripts/tidy.sh.
#
# Usage: tidy-size-ratchet.sh <size.txt> <baseline> <checked> <sharded 0|1> <update 0|1>
#   size.txt  the run's readability-function-size and -cognitive-complexity warnings
#   baseline  one "<file><TAB><function>" line per function allowed past a threshold
#   checked   the sources this run checked, one per line, relative to the root
#
# Fails (exit 1) on a function past a threshold that the baseline doesn't list,
# on a size line this script can't read (a dropped line would pass unseen),
# and on a baseline line that no longer applies: its function is within the
# thresholds or gone, or its file is gone. A shard judges only the lines of
# its own sources: a header's function can be seen by any shard, so a header's
# line is judged by an unsharded run alone. update=1 (unsharded only) removes
# the lines that no longer apply; a line is never added by the script.
# A name that repeats in a file (an overload, another class's method) has a
# baseline line for each function: lines are counted, not just compared.
set -u
size=$1 base=$2 checked=$3 sharded=$4 update=$5
export LC_ALL=C

pat="^([^:]+):([0-9]+):[0-9]+: warning: function '(.*)' (exceeds|has cognitive).*"
# One function is one file:line:name, whether it trips the size check, the
# complexity check or both; the line number is dropped after that
seen=$(sed -nE "s/$pat/\1\t\2\t\3/p" "$size" | sort -u | cut -f1,3 | sort)
# size.txt holds only the two size checks' warnings, so a line that fails pat
# (a lambda is worded "lambda has ...", not "function '...'") is unreadable
unreadable=$(grep -vE "$pat" "$size" | grep .)
listed=$(tr -d '\r' < "$base" | grep -vE '^[[:space:]]*(#|$)' | sort)

new=$(comm -23 <(printf '%s\n' "$seen") <(printf '%s\n' "$listed") | grep .)
unseen=$(comm -13 <(printf '%s\n' "$seen") <(printf '%s\n' "$listed") | grep .)

# The directories this run had sources in: a header's line is judged only
# where all its includers could have been checked
dirs=$(sed -E 's#/.*##' "$checked" | sort -u)
stale=
while IFS=$'\t' read -r file name; do
    [ -n "$file" ] || continue
    if [ ! -f "$file" ]; then
        stale="$stale$file	$name	(no such file)
"
    elif grep -qxF -- "$file" "$checked"; then
        stale="$stale$file	$name	(within the thresholds, or gone)
"
    elif [ "$sharded" = 0 ] && printf '%s\n' "$dirs" | grep -qxF -- "${file%%/*}"; then
        stale="$stale$file	$name	(within the thresholds, or gone)
"
    fi
done <<< "$unseen"
stale=$(printf '%s' "$stale" | grep .)

if [ "$update" = 1 ] && [ -n "$stale" ]; then
    drop=$(mktemp)
    printf '%s\n' "$stale" | cut -f1,2 > "$drop"
    n=$(grep -c . "$drop")
    # One baseline line per stale line: a repeated name keeps its other lines
    awk -F'\t' 'NR == FNR { gone[$1 FS $2]++; next }
        { k = $1 FS $2; if (gone[k] > 0) { gone[k]--; next } print }' "$drop" "$base" > "$base.new" && mv "$base.new" "$base"
    rm -f "$drop"
    echo "tidy        removed $n baseline line(s) that no longer apply from $base"
    stale=
fi

rc=0
if [ -n "$unreadable" ]; then
    n=$(printf '%s\n' "$unreadable" | wc -l | tr -d ' ')
    printf 'tidy        FAIL  %s size line(s) in %s this script cannot read (a drive letter in the path, a changed message):\n' "$n" "$size"
    printf '%s\n' "$unreadable" | sed 's/^/  /'
    rc=1
fi
if [ -n "$new" ]; then
    n=$(printf '%s\n' "$new" | wc -l | tr -d ' ')
    printf 'tidy        FAIL  %s function(s) past a size threshold (80 lines, cognitive complexity 40) and not in %s:\n' "$n" "$base"
    while IFS=$'\t' read -r file name; do
        why=$(grep -F -- "$file:" "$size" | grep -F -- "function '$name' " |
            sed -nE 's/.*(exceeds recommended size).*/size/p; s/.*cognitive complexity of ([0-9]+).*/complexity \1/p' | paste -sd, - | sed 's/,/, /g')
        printf '  %s  %s  (%s)\n' "$file" "$name" "$why"
    done <<< "$new"
    echo "  split it (a commit of moves) or, for a function that can't be cut now, add the line to the baseline by hand"
    rc=1
fi
if [ -n "$stale" ]; then
    n=$(printf '%s\n' "$stale" | wc -l | tr -d ' ')
    printf 'tidy        FAIL  %s line(s) of %s no longer apply:\n' "$n" "$base"
    printf '%s\n' "$stale" | awk -F'\t' '{ printf "  %s  %s  %s\n", $1, $2, $3 }'
    echo "  remove them (scripts/tidy.sh --update-size-baseline does)"
    rc=1
fi
exit $rc

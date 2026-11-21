#!/usr/bin/env bash
# scripts/tidy-size-ratchet.sh: judges clang-tidy's size and complexity
# lines against scripts/tidy-size-baseline.txt, for scripts/tidy.sh.
#
# Usage: tidy-size-ratchet.sh <size.txt> <baseline> <checked> <sharded 0|1> <update 0|1> [<notes.txt>]
#   size.txt   the run's readability-function-size and -cognitive-complexity warnings
#   baseline   one "<file><TAB><function><TAB><lines><TAB><complexity>" line per function
#              allowed past a threshold; a measure the function is within is "-"
#   checked    the sources this run checked, one per line, relative to the root
#   notes.txt  the size check's "note: N lines including whitespace and comments" lines
#
# Fails (exit 1) on a function past a threshold that the baseline doesn't list,
# on a size line this script can't read (a dropped line would pass unseen),
# and on a baseline line that no longer applies: its function is within the
# thresholds or gone, or its file is gone. A shard judges only the lines of
# its own sources: a header's function can be seen by any shard, so a header's
# line is judged by an unsharded run alone. A listed function also fails when
# its lines or its complexity is past the baseline's number, or past a
# threshold its line lists it within ("-"): a listed function can only shrink.
# update=1 (unsharded only) removes the lines that no longer apply and lowers
# a number that shrank; a line is never added, nor a number raised.
# A name that repeats in a file (an overload, another class's method) has a
# baseline line for each function: lines are counted, longest paired first.
set -u
size=$1 base=$2 checked=$3 sharded=$4 update=$5 notes=${6:-/dev/null}
export LC_ALL=C
tab=$(printf '\t')

pat="^([^:]+):([0-9]+):[0-9]+: warning: function '(.*)' (exceeds|has cognitive).*"
# One function is one file:line:col, whether it trips the size check, the
# complexity check or both. measured: file, name, lines, complexity (a
# measure the function is within is "-"), longest first within a name.
measured=$(awk '
    FILENAME == ARGV[1] {
        if ($0 ~ /^[^:]+:[0-9]+:[0-9]+: note: [0-9]+ lines including/) {
            k = $0; sub(/: note: .*/, "", k)
            n = $0; sub(/^[^:]+:[0-9]+:[0-9]+: note: /, "", n); sub(/ lines.*/, "", n)
            lines[k] = n
        }
        next
    }
    $0 ~ /^[^:]+:[0-9]+:[0-9]+: warning: function / {
        k = $0; sub(/: warning: .*/, "", k)
        f = k; sub(/:[0-9]+:[0-9]+$/, "", f)
        name = $0; sub(/^[^:]+:[0-9]+:[0-9]+: warning: function \047/, "", name)
        sub(/\047 (exceeds|has cognitive).*/, "", name)
        file[k] = f; fn[k] = name
        if ($0 ~ /has cognitive complexity of /) {
            c = $0; sub(/.*complexity of /, "", c); sub(/ .*/, "", c); cx[k] = c
        }
    }
    END {
        for (k in fn)
            printf "%s\t%s\t%s\t%s\n", file[k], fn[k], (k in lines ? lines[k] : "-"), (k in cx ? cx[k] : "-")
    }
' "$notes" "$size" | sort -t "$tab" -k1,1 -k2,2 -k3,3nr -k4,4nr)
seen=$(printf '%s\n' "$measured" | cut -f1,2 | sort)
# size.txt holds only the two size checks' warnings, so a line that fails pat
# (a lambda is worded "lambda has ...", not "function '...'") is unreadable
unreadable=$(grep -vE "$pat" "$size" | grep .)
baselines=$(tr -d '\r' < "$base" | grep -vE '^[[:space:]]*(#|$)' | sort -t "$tab" -k1,1 -k2,2 -k3,3nr -k4,4nr)
listed=$(printf '%s\n' "$baselines" | cut -f1,2 | sort)

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
    baselines=$(tr -d '\r' < "$base" | grep -vE '^[[:space:]]*(#|$)' | sort -t "$tab" -k1,1 -k2,2 -k3,3nr -k4,4nr)
fi

# Each baseline line against the run's measure of the function it pairs with
# (the nth longest of a name with the nth longest line): "grown" rows are
# file, name, the baseline's lines/complexity, the run's; "lowered" rows are
# the baseline line with its numbers cut to the run's. A baseline line with
# no numbers reads as "-" for both, so it grows on the first run.
paired=$(awk -F'\t' '
    function num(x) { return x == "-" || x == "" ? 0 : x + 0 }
    FILENAME == ARGV[1] { key = $1 FS $2; ns[key]++; sl[key, ns[key]] = $3; sc[key, ns[key]] = $4; next }
    {
        key = $1 FS $2; nb[key]++; i = nb[key]
        if (!(key SUBSEP i in sl)) next
        bl = ($3 == "" ? "-" : $3); bc = ($4 == "" ? "-" : $4)
        l = sl[key, i]; c = sc[key, i]
        grown = (num(l) > num(bl) || num(c) > num(bc))
        nl = (num(l) < num(bl) ? l : bl); nc = (num(c) < num(bc) ? c : bc)
        printf "%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\n", (grown ? "grown" : "ok"), $1, $2, bl, bc, l, c, nl, nc
    }
' <(printf '%s\n' "$measured") <(printf '%s\n' "$baselines"))
grown=$(printf '%s\n' "$paired" | awk -F'\t' '$1 == "grown"' | grep .)

if [ "$update" = 1 ]; then
    # Lowered numbers are written back; a line the run didn't see (a shard,
    # or a header) keeps its own
    low=$(printf '%s\n' "$paired" | awk -F'\t' '$1 == "ok" && ($4 != $8 || $5 != $9) { n++ } END { print n + 0 }')
    if [ "$low" != 0 ]; then
        out=$(mktemp)
        grep -E '^[[:space:]]*(#|$)' "$base" > "$out"
        { printf '%s\n' "$paired" | awk -F'\t' '{ printf "%s\t%s\t%s\t%s\n", $2, $3, $8, $9 }'
          # lines the run did not pair keep their text
          awk -F'\t' 'NR == FNR { nb[$2 FS $3]++; next }
              { k = $1 FS $2; seen[k]++; if (seen[k] > nb[k]) print }' \
              <(printf '%s\n' "$paired") <(printf '%s\n' "$baselines")
        } | sort -t "$tab" -k1,1 -k2,2 -k3,3nr -k4,4nr >> "$out"
        mv "$out" "$base"
        echo "tidy        lowered $low baseline number(s) in $base"
    fi
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
        why=$(printf '%s\n' "$measured" | awk -F'\t' -v f="$file" -v n="$name" '$1 == f && $2 == n {
            if ($3 != "-") printf "%s lines ", $3; if ($4 != "-") printf "complexity %s", $4; printf ";" }' | sed 's/ ;/;/g; s/;$//')
        printf '  %s  %s  (%s)\n' "$file" "$name" "$why"
    done <<< "$new"
    echo "  split it (a commit of moves) or, for a function that can't be cut now, add the line to the baseline by hand"
    rc=1
fi
if [ -n "$grown" ]; then
    n=$(printf '%s\n' "$grown" | wc -l | tr -d ' ')
    printf 'tidy        FAIL  %s listed function(s) of %s grew (lines/complexity, "-" within the threshold):\n' "$n" "$base"
    printf '%s\n' "$grown" | awk -F'\t' '{ printf "  %s  %s  %s/%s -> %s/%s\n", $2, $3, $4, $5, $6, $7 }'
    echo "  a listed function only shrinks: cut it back, or split it (a commit of moves)"
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

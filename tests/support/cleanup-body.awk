# Prints the body of a test suite's cleanup(), wherever it is written.
#
# Both shapes exist in this tree: out of line as `void TestX::cleanup()` with the
# brace on the next line, and inline in the class as `void cleanup() { ... }` on
# one line. So this counts braces rather than reading indentation, the same way
# deferred-expansion.awk does -- a checker that guessed from the layout would
# quietly print nothing for half the suites and pass them all.
#
# A declaration (`void cleanup();`) is not a definition and is skipped, or the
# range would start at the class body and swallow everything after it.

/cleanup[ \t]*\([ \t]*\)/ {
    if (inside)
        next
    if ($0 ~ /;[ \t]*$/)   # a declaration, not the definition
        next
    inside = 1
    started = 0
    depth = 0
}

inside {
    print
    opens = gsub(/\{/, "{")
    closes = gsub(/\}/, "}")
    depth += opens - closes
    if (opens > 0)
        started = 1
    if (started && depth <= 0) {
        inside = 0
        started = 0
        depth = 0
    }
}

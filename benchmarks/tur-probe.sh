# benchmarks/tur-probe.sh -- sourced by the benchmark runners.
#
# require_unsanitized_tur <tur> <runner-name>
#   Exit 2 unless <tur> is a build without ASan/TSan/UBSan.  A Debug tur is
#   built -fsanitize=address,undefined and the programs it builds link the
#   sanitized libturi, so every number would be shadow-memory bookkeeping.
#   Set ALLOW_SANITIZED_TUR=1 to measure one anyway (for eyeballing, never for
#   rows that get checked in).
#
#   The probe reads symbol tables with nm, as the driver's own libturi
#   autodetect does (src/main.c).  `grep __asan_init` on the binary is NOT a
#   substitute: a Release tur contains that string, because the driver carries
#   the nm command line as a literal.
require_unsanitized_tur() {
    local tur="$1" who="$2" syms
    if ! command -v nm >/dev/null 2>&1; then
        echo "$who: WARNING -- no nm on PATH, cannot check $tur for sanitizers" >&2
        return 0
    fi
    syms=$({ nm "$tur"; nm -D "$tur"; } 2>/dev/null \
           | grep -oE '__(asan_init|tsan_init|ubsan_handle_)' | sort -u | tr '\n' ' ')
    [ -z "$syms" ] && return 0
    if [ "${ALLOW_SANITIZED_TUR:-0}" = 1 ]; then
        echo "$who: WARNING -- $tur is sanitized ($syms); these numbers are not results" >&2
        return 0
    fi
    echo "$who: $tur is sanitized ($syms) -- its numbers would be sanitizer overhead." >&2
    echo "  Build a Release tur:" >&2
    echo "    cmake -S . -B build-release -DCMAKE_BUILD_TYPE=Release -DTUR_JIT=ON -DCMAKE_POLICY_VERSION_MINIMUM=3.5" >&2
    echo "    cmake --build build-release -j" >&2
    echo "  or set ALLOW_SANITIZED_TUR=1 to measure it anyway." >&2
    exit 2
}

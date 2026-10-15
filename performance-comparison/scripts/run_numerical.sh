#!/bin/bash
set -euo pipefail

# Configuration
BENCHMARK_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/../benchmarks/numerical" && pwd)"
INPUT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/../inputs/numerical" && pwd)"
RESULTS_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/../results/raw" && pwd)"

# Ensure results directory exists
mkdir -p "$RESULTS_DIR"

# Benchmark functions
run_benchmark() {
    local language=$1
    local benchmark=$2
    local input_file=$3
    local output_file="${RESULTS_DIR}/${benchmark}_${language}_$(date +%Y%m%d_%H%M%S).json"

    echo "Running $language $benchmark benchmark..."
    
    case $language in
        c)
            cd "${BENCHMARK_DIR}/c"
            clang -O3 -o ${benchmark} ${benchmark}.c
            /usr/bin/time -v ./${benchmark} "$(jq -r .small.n $input_file)" > >(jq -n --arg result "$(./${benchmark} "$(jq -r .small.n $input_file)" | tail -1)" \
                --arg time "$(/usr/bin/time -v ./${benchmark} "$(jq -r .small.n $input_file)" 2>&1 | grep 'Elapsed' | awk '{print $2}')" \
                '{"language": "c", "benchmark": "${benchmark}", "implementation": "iterative", "input_size": "small", "result": $result, "time_s": $time}') > "$output_file"
            ;;
        turmeric)
            cd "${BENCHMARK_DIR}/turmeric"
            tur run release
            build-rel/tur ${benchmark}.tur "$(jq -r .small.n $input_file)" > "$output_file"
            ;;
        clojure)
            cd "${BENCHMARK_DIR}/clojure"
            clojure -M ${benchmark}.clj "$(jq -r .small.n $input_file)" > "$output_file"
            ;;
        racket)
            cd "${BENCHMARK_DIR}/racket"
            racket ${benchmark}.rkt "$(jq -r .small.n $input_file)" > "$output_file"
            ;;
        python)
            cd "${BENCHMARK_DIR}/python"
            python3 ${benchmark}.py "$(jq -r .small.n $input_file)" > "$output_file"
            ;;
    esac
    
    echo "Results saved to $output_file"
}

# Main execution
main() {
    local benchmarks=("fibonacci" "primes" "matrix" "monte_carlo")
    local languages=("c" "turmeric" "clojure" "racket" "python")

    for benchmark in "${benchmarks[@]}"; do
        input_file="${INPUT_DIR}/${benchmark}.json"
        
        for language in "${languages[@]}"; do
            if [ -f "${BENCHMARK_DIR}/${language}/${benchmark}.${language##*.}" ]; then
                run_benchmark "$language" "$benchmark" "$input_file"
            else
                echo "Skipping $language $benchmark - implementation not found"
            fi
        done
    done
}

main
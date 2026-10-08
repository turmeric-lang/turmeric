---
title: tur-stats Guide
category: Data Structures and Libraries
description: Statistics spice for Turmeric -- descriptive stats, probability distributions, hypothesis tests, OLS regression, and resampling on dataframes
---

# tur-stats Guide

`tur-stats` is the official statistics spice for Turmeric. It provides
descriptive statistics, probability distributions, hypothesis tests, OLS
regression, and resampling utilities -- all operating on `tur-frame`
dataframes and raw float64 columns.

---

## Installation

Add `tur-stats` as a dependency in your `build.tur`:

```turmeric no-check
:spices #map{
  "frame" #map{:url    "https://github.com/turmeric-lang/turmeric-spices"
               :ref    "frame-v0.1.0"
               :subdir "spices/frame"}
  "stats" #map{:url    "https://github.com/turmeric-lang/turmeric-spices"
               :ref    "stats-v0.1.0"
               :subdir "spices/stats"}
}
```
```sweet-exp
:spices
#map{
  "frame" #map{:url    "https://github.com/turmeric-lang/turmeric-spices"
               :ref    "frame-v0.1.0"
               :subdir "spices/frame"}
  "stats" #map{:url    "https://github.com/turmeric-lang/turmeric-spices"
               :ref    "stats-v0.1.0"
               :subdir "spices/stats"}
}
```

Or from the command line:

```sh
tur add https://github.com/turmeric-lang/turmeric-spices \
  --ref stats-v0.1.0 --subdir spices/stats --name stats
```

---

## Modules

| Module | Contents |
|--------|----------|
| `stats/mathx` | C stdlib math wrappers (erf, lgamma, expm1, ...) |
| `stats/rng` | PCG32 pseudo-random number generator |
| `stats/summary` | Descriptive statistics: mean, median, sd, quantiles, ... |
| `stats/cov` | Covariance, correlation, Spearman, correlation matrices |
| `stats/dist` | 10 probability distributions: pdf / cdf / quantile / random |
| `stats/test` | Hypothesis tests: t-tests, ANOVA, chi-squared, KS, ... |
| `stats/regress` | OLS linear regression via Cholesky factorization |
| `stats/sample` | Bootstrap, permutation test, train-test-split, k-fold CV |
| `stats/fmt` | Pretty-printing for test-result and lm-fit |

---

## Quick start

```turmeric
(import stats/summary :refer [col-mean col-sd describe])
(import stats/dist    :refer [pnorm qnorm])
(import stats/test    :refer [t-test-1samp alt-two-sided])
(import stats/fmt     :refer [print-test])
(import stats/rng     :refer [rng-make rng-free])

;; Build a float64 column (see tur-frame guide for helpers).
;; Assume `my-col` is already a frame column handle.

(let [m (col-mean my-col)
      s (col-sd   my-col)]
  (println (str-concat "mean=" (float->str m) " sd=" (float->str s))))

;; Standard normal CDF / quantile.
(pnorm 1.96 0.0 1.0)  ; => ~0.975
(qnorm 0.975 0.0 1.0) ; => ~1.96

;; One-sample t-test.
(let [r (t-test-1samp my-col 0.0 (alt-two-sided) 0.95)]
  (print-test r))
```
```sweet-exp
import stats/summary :refer [col-mean col-sd describe]
import stats/dist :refer [pnorm qnorm]
import stats/test :refer [t-test-1samp alt-two-sided]
import stats/fmt :refer [print-test]
import stats/rng :refer [rng-make rng-free]
;; Build a float64 column (see tur-frame guide for helpers).
;; Assume `my-col` is already a frame column handle.
let [m (col-mean my-col)
      s (col-sd   my-col)]
  println(str-concat("mean=" float->str(m) " sd=" float->str(s)))
;; Standard normal CDF / quantile.
pnorm(1.96 0.0 1.0)
; => ~0.975
qnorm(0.975 0.0 1.0)
; => ~1.96
;; One-sample t-test.
let [r (t-test-1samp my-col 0.0 (alt-two-sided) 0.95)]
  print-test(r)
```

---

## stats/rng

The random-number generator is an explicit state object backed by
[PCG32](https://www.pcg-random.org/). You must create one before any
function that needs randomness.

```turmeric
(import stats/rng :refer [rng-make rng-free rng-uniform rng-int-range
                           rng-shuffle! rng-sample])

(let [rng (rng-make 42 1)]      ; seed=42, stream=1
  (rng-uniform rng)             ; => double in [0,1)
  (rng-int-range rng 0 100)     ; => int in [0,100)
  (rng-shuffle! rng my-list)    ; => shuffled list (new allocation)
  (rng-sample rng my-list 3)    ; => 3-element list (without replacement)
  (rng-free rng))
```
```sweet-exp
import stats/rng :refer [rng-make rng-free rng-uniform rng-int-range
                           rng-shuffle! rng-sample]
let [rng (rng-make 42 1)]
  ; seed=42, stream=1
  rng-uniform(rng)
  ; => double in [0,1)
  rng-int-range(rng 0 100)
  ; => int in [0,100)
  rng-shuffle!(rng my-list)
  ; => shuffled list (new allocation)
  rng-sample(rng my-list 3)
  ; => 3-element list (without replacement)
  rng-free(rng)
```

---

## stats/summary

All `col-*` functions take a `column<:float>` handle (`:int`).
All `frame-*` functions take a frame handle plus a column name (`:cstr`).

```turmeric
(import stats/summary :refer [col-mean col-median col-sd col-var
                               col-min col-max col-quantile col-iqr
                               col-skewness col-kurtosis describe])

(col-mean col)          ; arithmetic mean
(col-median col)        ; sample median (sorts internally)
(col-sd col)            ; sample std dev (ddof=1)
(col-quantile col 0.25) ; first quartile
(col-iqr col)           ; interquartile range

;; describe returns a frame with rows: count mean sd min q25 median q75 max
;; and one column per numeric column in the source frame.
(describe my-frame)
```
```sweet-exp
import stats/summary :refer [col-mean col-median col-sd col-var
                               col-min col-max col-quantile col-iqr
                               col-skewness col-kurtosis describe]
col-mean(col)
; arithmetic mean
col-median(col)
; sample median (sorts internally)
col-sd(col)
; sample std dev (ddof=1)
col-quantile(col 0.25)
; first quartile
col-iqr(col)
; interquartile range
;; describe returns a frame with rows: count mean sd min q25 median q75 max
;; and one column per numeric column in the source frame.
describe(my-frame)
```

---

## stats/dist

Ten distributions are supported: normal, t, chi2, F, uniform, binomial,
Poisson, exponential, beta, gamma.

Each distribution has four operations:
- `pdf` -- probability density (or mass) function
- `cdf` -- cumulative distribution function
- `quantile` -- inverse CDF
- `random` / `random-n` -- random variate(s)

**Generic API** (polymorphic over distribution):

```turmeric
(import stats/dist :refer [dist-normal pdf cdf quantile random random-n])
(import stats/rng  :refer [rng-make rng-free])

(let [d   (dist-normal 0.0 1.0)  ; mean=0, sd=1
      rng (rng-make 0 0)]
  (pdf      d 1.0)               ; 0.2420
  (cdf      d 1.96)              ; 0.975
  (quantile d 0.975)             ; 1.96
  (random   d rng)               ; one sample
  (random-n d rng 100)           ; column of 100 samples
  (rng-free rng))
```
```sweet-exp
import stats/dist :refer [dist-normal pdf cdf quantile random random-n]
import stats/rng :refer [rng-make rng-free]
let [d   (dist-normal 0.0 1.0)  ; mean=0, sd=1
      rng (rng-make 0 0)]
  pdf(d 1.0)
  ; 0.2420
  cdf(d 1.96)
  ; 0.975
  quantile(d 0.975)
  ; 1.96
  random(d rng)
  ; one sample
  random-n(d rng 100)
  ; column of 100 samples
  rng-free(rng)
```

**Convenience wrappers** (R-style, named per distribution):

```turmeric
(import stats/dist :refer [dnorm pnorm qnorm rnorm
                            dt pt qt rt
                            dchi2 pchi2 qchi2 rchi2
                            df-dist pf-dist qf-dist rf-dist
                            dunif punif qunif runif
                            dbinom pbinom qbinom rbinom
                            dpois ppois qpois rpois
                            dexp pexp qexp rexp
                            dbeta pbeta qbeta rbeta
                            dgamma pgamma qgamma rgamma])

(pnorm 1.96 0.0 1.0)      ; CDF of N(0,1) at 1.96
(qnorm 0.975 0.0 1.0)     ; 97.5th percentile
(rbinom 0 10 0.3 rng)     ; one Binomial(n=10, p=0.3) variate
```
```sweet-exp
import stats/dist :refer [dnorm pnorm qnorm rnorm
                            dt pt qt rt
                            dchi2 pchi2 qchi2 rchi2
                            df-dist pf-dist qf-dist rf-dist
                            dunif punif qunif runif
                            dbinom pbinom qbinom rbinom
                            dpois ppois qpois rpois
                            dexp pexp qexp rexp
                            dbeta pbeta qbeta rbeta
                            dgamma pgamma qgamma rgamma]
pnorm(1.96 0.0 1.0)
; CDF of N(0,1) at 1.96
qnorm(0.975 0.0 1.0)
; 97.5th percentile
rbinom(0 10 0.3 rng)
; one Binomial(n=10, p=0.3) variate
```

The `r*` family (except `rnorm`, `runif`, `rexp`, `rbeta`, `rgamma`)
uses the inverse-CDF method.

---

## stats/test

All tests return a `test-result` value. Use `print-test` from `stats/fmt`
to display results in R-style.

```turmeric
(import stats/test :refer [t-test-1samp t-test-2samp t-test-paired
                             anova-oneway chi2-gof chi2-contingency
                             var-test cor-test
                             mann-whitney wilcoxon-signed-rank
                             ks-test-1samp ks-test-2samp
                             alt-two-sided alt-less alt-greater
                             test-result-statistic test-result-p-value
                             test-result-ci-low test-result-ci-high])
(import stats/fmt  :refer [print-test])

;; One-sample t-test: H0: mean(col) == 0
(let [r (t-test-1samp col 0.0 (alt-two-sided) 0.95)]
  (print-test r))

;; Two-sample Welch t-test
(let [r (t-test-2samp group-a group-b 0 (alt-two-sided) 0.95)]
  (test-result-p-value r))

;; One-way ANOVA
;; cols = list of group columns
(anova-oneway (list g1 g2 g3))

;; Chi-squared goodness-of-fit
(chi2-gof observed-col expected-col)

;; Kolmogorov-Smirnov (one sample: compare to a distribution)
(import stats/dist :refer [dist-normal])
(ks-test-1samp col (dist-normal 0.0 1.0) (alt-two-sided))
```
```sweet-exp
import stats/test :refer [t-test-1samp t-test-2samp t-test-paired
                             anova-oneway chi2-gof chi2-contingency
                             var-test cor-test
                             mann-whitney wilcoxon-signed-rank
                             ks-test-1samp ks-test-2samp
                             alt-two-sided alt-less alt-greater
                             test-result-statistic test-result-p-value
                             test-result-ci-low test-result-ci-high]
import stats/fmt :refer [print-test]
;; One-sample t-test: H0: mean(col) == 0
let [r (t-test-1samp col 0.0 (alt-two-sided) 0.95)]
  print-test(r)
;; Two-sample Welch t-test
let [r (t-test-2samp group-a group-b 0 (alt-two-sided) 0.95)]
  test-result-p-value(r)
;; One-way ANOVA
;; cols = list of group columns
anova-oneway(list(g1 g2 g3))
;; Chi-squared goodness-of-fit
chi2-gof(observed-col expected-col)
;; Kolmogorov-Smirnov (one sample: compare to a distribution)
import stats/dist :refer [dist-normal]
ks-test-1samp(col dist-normal(0.0 1.0) alt-two-sided())
```

### Accessing test-result fields

```turmeric
(test-result-name      r)  ; :cstr -- test name
(test-result-statistic r)  ; :float -- test statistic
(test-result-df1       r)  ; :float -- primary df (NaN if absent)
(test-result-p-value   r)  ; :float -- p-value
(test-result-estimate  r)  ; :float -- point estimate
(test-result-ci-low    r)  ; :float -- CI lower bound
(test-result-ci-high   r)  ; :float -- CI upper bound
(test-result-conf-level r) ; :float -- confidence level (e.g. 0.95)
```
```sweet-exp
test-result-name(r)
; :cstr -- test name
test-result-statistic(r)
; :float -- test statistic
test-result-df1(r)
; :float -- primary df (NaN if absent)
test-result-p-value(r)
; :float -- p-value
test-result-estimate(r)
; :float -- point estimate
test-result-ci-low(r)
; :float -- CI lower bound
test-result-ci-high(r)
; :float -- CI upper bound
test-result-conf-level(r)
; :float -- confidence level (e.g. 0.95)
```

---

## stats/regress

OLS via normal equations solved by Cholesky factorization.

```turmeric
(import stats/regress :refer [ols ols-frame predict predict-frame diagnostics])
(import stats/fmt     :refer [print-fit fit-coefs-frame])

;; xs = list of predictor columns.
;; names = list of :cstr labels.
(let [result (ols y-col xs names 1)]  ; 1 = include intercept
  (if (ok? result)
    (let [fit (ok-val result)]
      (print-fit fit)
      (fit-coefs-frame fit))   ; => frame with name/estimate/se/t/p
    (println (err-val result))))

;; Frame-based interface.
(let [result (ols-frame my-frame "y" (list "x1" "x2") 1)]
  ...)

;; Predictions.
(predict fit (list new-x1 new-x2))    ; => column
(predict-frame fit new-frame)                   ; => result<column>

;; Diagnostics frame: fitted, residual, std-residual, leverage, cooks-d.
(diagnostics fit)
```
```sweet-exp
import stats/regress :refer [ols ols-frame predict predict-frame diagnostics]
import stats/fmt :refer [print-fit fit-coefs-frame]
;; xs = list of predictor columns.
;; names = list of :cstr labels.
let [result (ols y-col xs names 1)]
  ; 1 = include intercept
  if ok?(result)
    let [fit (ok-val result)]
      print-fit(fit)
      fit-coefs-frame(fit)
    ; => frame with name/estimate/se/t/p
    println(err-val(result))
;; Frame-based interface.
let [result (ols-frame my-frame "y" (list "x1" "x2") 1)]
  ...
;; Predictions.
predict(fit list(new-x1 new-x2))
; => column
predict-frame(fit new-frame)
; => result<column>
;; Diagnostics frame: fitted, residual, std-residual, leverage, cooks-d.
diagnostics(fit)
```

> **Note:** `lm-fit` is a heap-allocated C struct. Memory management is
> manual (same as `test-result`). Use `lm-fit-free` once implemented, or
> let it live for the duration of the program for short scripts.

---

## stats/sample

```turmeric
(import stats/sample :refer [bootstrap bootstrap-2samp permutation-test
                               train-test-split cv-folds cv-folds-stratified])
(import stats/rng    :refer [rng-make rng-free])

(let [rng (rng-make 42 0)]

  ;; Bootstrap CI for the mean (tag 0 = mean).
  (let [r (bootstrap col 0 1000 0.95 0 rng)]
    (print-test r))

  ;; Permutation test.
  (let [r (permutation-test group-a group-b 0 5000 (alt-two-sided) rng)]
    (test-result-p-value r))

  ;; Train-test split (20% test, no stratification).
  (let [res (train-test-split my-frame 0.2 0 rng)]
    (if (ok? res)
      (let [pair   (ok-val res)
            train  (car pair)
            test   (cdr pair)]
        ...)))

  ;; 5-fold CV.
  (let [folds (cv-folds 100 5 1 rng)]
    ;; folds = list of (cons train-indices test-indices)
    ...)

  (rng-free rng))
```
```sweet-exp
import stats/sample :refer [bootstrap bootstrap-2samp permutation-test
                               train-test-split cv-folds cv-folds-stratified]
import stats/rng :refer [rng-make rng-free]
let [rng (rng-make 42 0)]
  ;; Bootstrap CI for the mean (tag 0 = mean).
  let [r (bootstrap col 0 1000 0.95 0 rng)]
    print-test(r)
  ;; Permutation test.
  let [r (permutation-test group-a group-b 0 5000 (alt-two-sided) rng)]
    test-result-p-value(r)
  ;; Train-test split (20% test, no stratification).
  let [res (train-test-split my-frame 0.2 0 rng)]
    if ok?(res)
      let [pair   (ok-val res)
            train  (car pair)
            test   (cdr pair)]
        ...
  ;; 5-fold CV.
  let [folds (cv-folds 100 5 1 rng)]
    ;; folds = list of (cons train-indices test-indices)
    ...
  rng-free(rng)
```

### Statistic function tags for bootstrap

`stat-fn` is passed as an `:int` tag:

| Tag | Statistic |
|-----|-----------|
| `0` | mean (default) |
| `1` | standard deviation |

Full closure-based stat-fn support is planned for a future release.

---

## stats/fmt

```turmeric
(import stats/fmt :refer [print-test test->str print-fit fit->str fit-coefs-frame])

;; Print a test-result (R-style output).
(print-test r)      ; to stdout
(test->str r)       ; => :cstr  (caller frees)

;; Print an lm-fit summary.
(print-fit fit)     ; to stdout
(fit->str fit)      ; => :cstr  (caller frees)

;; Extract coefficient table as a frame.
(fit-coefs-frame fit)  ; => frame with columns: name, estimate, se, t, p
```
```sweet-exp
import stats/fmt :refer [print-test test->str print-fit fit->str fit-coefs-frame]
;; Print a test-result (R-style output).
print-test(r)
; to stdout
test->str(r)
; => :cstr  (caller frees)
;; Print an lm-fit summary.
print-fit(fit)
; to stdout
fit->str(fit)
; => :cstr  (caller frees)
;; Extract coefficient table as a frame.
fit-coefs-frame(fit)
; => frame with columns: name, estimate, se, t, p
```

---

## Numerical notes

- Normal quantile uses the Beasley-Springer-Moro rational approximation
  (error < 1e-9 over the central region).
- Incomplete beta and gamma use Lentz's continued-fraction algorithm
  (300 iterations, tolerance 3e-8).
- OLS detects rank deficiency via a Cholesky pivot threshold of
  `1e-10 * max(diag(X'X))` and returns `err "rank-deficient design matrix..."`.
- Mann-Whitney and Wilcoxon signed-rank use normal approximations (valid
  for n > ~20); no exact permutation distributions in v0.1.0.
- KS p-values use the Kolmogorov asymptotic formula (100 terms).

---

## Limitations (v0.1.0)

- `bootstrap` / `bootstrap-2samp` stat-fn is an integer tag, not a
  first-class closure.
- `diagnostics` uses average leverage `p/n` rather than exact hat-matrix
  diagonals (exact leverage requires storing the full design matrix).
- `train-test-split` stratification (`stratify-col != 0`) is not yet
  implemented; it falls back to simple random split.
- No streaming or chunked computation; all data must fit in RAM.

# tur-stats: `ols` / `ols-frame` p-values come from a wrong continued fraction (one is negative)

**Severity: medium (wrong numbers in a fitted model's summary).** Found
2026-10-29 executing stats-formula-plan, fitting iris through
`spices/stats-formula`.

## Repro

`lm(Sepal.Length ~ Species * Petal.Length, iris)` through
`formula-parse-terms` + `ols-frame` + `print-fit` (the coefficients are
right, matching R to 1e-6):

```
Speciesvirginica:Petal.Length   0.453446   0.290146   1.5628    -0.8967 ***
```

R gives p = 0.1203 for that row (t = 1.5628, 144 df). The other rows are
close to R's but not equal: `mpg ~ wt * hp` prints 4.027e-05 and 0.000806
where R prints 4.04e-05 and 0.000811.

## Root cause

`spices/stats/src/stats/regress.tur` computes each p-value inline as
I_x(df/2, 1/2) with x = df/(df + t^2), through a hand-written Lentz
continued fraction (`_IBETA_CF` in `ols`, `_IB2` in `ols-frame`). Its terms
are not the incomplete-beta fraction's: the even term should be
m(b - m)x / ((a + 2m - 1)(a + 2m)), and it computes `_bb*_xa*m/...` (b, not
b - m); the odd term should be -(a + m)(a + b + m)x / ((a + 2m)(a + 2m + 1)),
and it computes a positive `_aa*_xa*m/...`. The loop also starts at m = 0.
`stats/dist`'s `pt` is correct (it agrees with numerical integration to 6
digits for these t and df), so the regression code is the only consumer of
the bad fraction.

## Fix directions

- Compute p = 2 * (1 - pt(|t|, df)) with `stats/dist`'s implementation
  (inline-C cannot call it, so share the C helper), in both `ols` and
  `ols-frame`.
- A regression test with R's p-values for mtcars `mpg ~ wt * hp` and the
  iris row above.

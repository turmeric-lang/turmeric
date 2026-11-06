# tur-stats: `ols` / `ols-frame` p-values come from a wrong continued fraction (one is negative)

**RESOLVED 2026-11-05** in turmeric-spices [#134](https://github.com/turmeric-lang/turmeric-spices/pull/134) -- see "Resolution" at the end.

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

## Resolution (2026-11-05)

Fix direction 1, with the helper local to the module instead of shared:
`ols` and `ols-frame` now call `__stats_rg_p2(t, df)`, which is
`__stats_rg_ibeta(df/(df+t^2), df/2, 1/2)`, in `regress.tur`'s module-level C
block. The fraction has the same terms as `stats/dist`'s `__ibeta`; inline C
cannot call that one, and `regress.tur` should not depend on another module's
C block being emitted first.

`tests/stats/regress_test.tur` pins a 5-point fit's two p-values against a
Simpson integration of the t density. It fails on the old fraction. The same
integration gives 0.12029 for the iris row above (R: 0.1203), and it matches
every p-value the `stats-formula` tour prints for `mpg ~ wt + fuel`.

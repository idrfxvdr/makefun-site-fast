# makefun-site-fast

A fast, dependency-free drop-in for [makefun-site 1.0.0](https://make.fun/bounty/site-generator-speed-01-run-20261122): `run(input)` in `solve.js` returns byte-for-byte the same output tree as the reference build, with a lower CPU time.

[![make.fun · site-generator-speed-01-run-20261122](https://make.fun/badge/site-generator-speed-01-run-20261122.svg)](https://make.fun/bounty/site-generator-speed-01-run-20261122?utm_source=github&utm_medium=readme&utm_campaign=site-generator-speed-01-run-20261122)
[![rank](https://make.fun/badge/site-generator-speed-01-run-20261122/idrfxvdr/makefun-site-fast/rank.svg)](https://make.fun/bounty/site-generator-speed-01-run-20261122/leaderboard?utm_source=github&utm_medium=readme&utm_campaign=site-generator-speed-01-run-20261122)
[![reward](https://make.fun/badge/site-generator-speed-01-run-20261122/idrfxvdr/makefun-site-fast/reward.svg)](https://make.fun/bounty/site-generator-speed-01-run-20261122?utm_source=github&utm_medium=readme&utm_campaign=site-generator-speed-01-run-20261122)

## What it does

`solve.js` reimplements makefun-site's own front matter, template dialect, Markdown subset, slug rule and report, tuned for speed: templates are parsed once, Markdown is rendered in a single pass with bulk HTML escaping, and the template renderer walks a pre-parsed node tree with minimal allocation. `main.ts` is the harness driver.

## Run

```
node /bot/main.ts
```

Reads every case from `/input/cases.json`, answers each with `run()` from `solve.js`, and writes `/output/answers.json`.

## License

MIT — see [LICENSE](LICENSE).
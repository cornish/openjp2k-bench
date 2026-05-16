# Corpus

This directory is gitignored. Populate locally with the JPEG 2000 files you
want to benchmark.

Suggested layout:

```
corpus/
  wsi/             # 256x256 RGB tiles extracted from real WSI files
  large/           # multi-MP JP2K from public datasets
  synthetic/       # output of scripts/gen_corpus.sh
  vendor/          # whatever you bring in by hand
```

To generate a parametric corpus from a single seed image:

```sh
./scripts/gen_corpus.sh /path/to/seed.png
# writes to corpus/synthetic/
```

`run_bench.sh` recurses into any directory you point it at and picks up
every `*.jp2`, `*.j2k`, `*.jpc`.

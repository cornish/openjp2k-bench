# corpus/

Inputs the bench runs against. Layout:

- `user/` — Files you drop in by hand. Not tracked, not generated.
- `synthetic/` — Output of `scripts/gen_corpus.sh` (parametric `opj_compress` grid). Not tracked, regenerate locally.
- `public/` — Files fetched from public sources (conformance / archival / remote-sensing / medical / cinema). **This repo expects `corpus/public/` to be a symlink to `~/GitHub/openjp2k-data/corpus/`**, the sibling repo that owns the curated fetcher and `MANUAL_SOURCES.md` for auth-gated sources.

To set it up:

```sh
git clone https://github.com/cornish/openjp2k-data ~/GitHub/openjp2k-data
~/GitHub/openjp2k-data/fetch_corpus.sh   # downloads what is auto-fetchable
ln -sfn ~/GitHub/openjp2k-data/corpus corpus/public
```

After populating any bucket, regenerate `corpus/manifest.json`:

```sh
./scripts/build_manifest.sh
```

The manifest records width / height / components / bit depth / tile dims / decomp levels / lossy-vs-lossless / progression / sha256 per file, parsed directly from SIZ + COD markers — no external deps. See `scripts/manifest_tool.py`.

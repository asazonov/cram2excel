# CRAM2Excel

![CRAM2Excel logo](assets/logo.svg)

`CRAM2Excel` is an HTSlib output plugin for SAMtools that turns `samtools view -h` output into a native Excel workbook.

Instead of streaming SAM to a text file, you can write directly to:

```bash
samtools view -h sample.cram -o cram2excel:sample.xlsx
```

The workbook contains:

- one or more `AlignmentsN` worksheets with standard SAM columns
- a `Header` worksheet with the original SAM header lines

## Why this exists

CRAM and BAM are perfect for pipelines, but not for quick handoff to analysts, clinicians, reviewers, or project managers who need a browsable spreadsheet. `CRAM2Excel` keeps the source-of-truth in SAMtools while giving you a lightweight export path for inspection and sharing.

## Features

- works as a native HTSlib plugin, so it fits SAMtools workflows naturally
- writes standard `.xlsx` files without a Python or Java dependency
- preserves SAM headers in a dedicated worksheet
- rolls over automatically to `Alignments2`, `Alignments3`, and so on when Excel row limits are reached
- keeps optional SAM tags together in a `TAGS` column
- validated end to end against SAMtools 1.23 and HTSlib 1.23

## Quick Start

1. Build the plugin:

```bash
make
```

2. Build a local plugin-enabled SAMtools toolchain:

```bash
make toolchain
```

3. Export a CRAM file to Excel:

```bash
HTS_PATH=$PWD/build \
./external/samtools-1.23/samtools view -h input.cram -o cram2excel:output.xlsx
```

4. Run the bundled validation/demo:

```bash
make validate
```

## Workbook Layout

`AlignmentsN` sheets use these columns:

- `QNAME`
- `FLAG`
- `RNAME`
- `POS`
- `MAPQ`
- `CIGAR`
- `RNEXT`
- `PNEXT`
- `TLEN`
- `SEQ`
- `QUAL`
- `TAGS`

`Header` is a single-column worksheet with one SAM header line per row.

## Documentation

- [Tutorial](docs/tutorial.md)
- [Examples](examples/README.md)
- [Contributing](CONTRIBUTING.md)

## Repository Layout

```text
.
├── assets/                  Brand assets and logo
├── docs/                    Tutorial and reference docs
├── examples/                Tiny reproducible demo inputs
├── scripts/                 Toolchain bootstrap helpers
├── .github/workflows/       GitHub Actions validation
├── hfile_cram2excel.c       Plugin source
├── Makefile                 Build entrypoint
└── validate.sh              End-to-end validation
```

## Notes

- This plugin expects textual SAM output, so use it with `samtools view` and not `-b` or `-C`.
- On macOS HTSlib discovers plugins as `hfile_*.bundle`; on Linux it expects `hfile_*.so`.
- Optional tag order may vary after CRAM round-trips because SAMtools can reorder emitted tags.

## License

MIT, see [LICENSE](LICENSE).

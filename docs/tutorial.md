# Tutorial

## What You Will Build

In this tutorial we will:

1. build `CRAM2Excel`
2. build a local SAMtools + HTSlib toolchain with plugin support
3. generate a tiny CRAM test file
4. export that CRAM to an Excel workbook
5. inspect the workbook structure

## Prerequisites

- `cc` or `gcc`
- `make`
- `curl`
- `tar`
- `unzip`
- zlib development headers

On Linux you will usually also want `libbz2-dev` and `liblzma-dev` available for HTSlib.

## Step 1: Build the Plugin

From the repository root:

```bash
make
```

This creates:

- macOS: `build/hfile_cram2excel.bundle`
- Linux: `build/hfile_cram2excel.so`

## Step 2: Build SAMtools with Plugins Enabled

```bash
make toolchain
```

That script downloads official HTSlib and SAMtools 1.23 source releases into `external/`, configures SAMtools with plugins enabled, and bakes the repository's `build/` directory into the plugin search path.

## Step 3: Use the Tiny Example

The repository ships a miniature example in [examples/tiny](../examples/tiny).

Run:

```bash
make validate
```

This does the following:

- indexes the reference FASTA
- converts the example SAM into a CRAM
- exports the CRAM into `build/validation/tiny.xlsx`
- checks that the workbook is structurally valid

## Step 4: Export Your Own CRAM

```bash
HTS_PATH=$PWD/build \
./external/samtools-1.23/samtools view -h your.cram -o cram2excel:your.xlsx
```

If your CRAM needs an external reference, use the normal SAMtools options:

```bash
HTS_PATH=$PWD/build \
./external/samtools-1.23/samtools view -h -T reference.fa your.cram -o cram2excel:your.xlsx
```

## What the Workbook Contains

`CRAM2Excel` writes:

- `Alignments1` for the first chunk of alignments
- `Alignments2`, `Alignments3`, ... if Excel's per-sheet row limit is exceeded
- `Header` for `@HD`, `@SQ`, `@RG`, `@PG`, and other SAM header records

Each alignment row includes the 11 standard SAM fields and a `TAGS` field that joins optional tags with ` | `.

## Troubleshooting

### “Protocol not supported”

HTSlib did not find the plugin.

Make sure:

- the plugin was built
- `HTS_PATH` points at `build/`
- on macOS the plugin filename ends in `.bundle`
- on Linux the plugin filename ends in `.so`

### “Expected SAM text from samtools view”

The plugin expects textual SAM output. Use:

```bash
samtools view -h input.cram -o cram2excel:output.xlsx
```

Do not use `-b` or `-C` on the export step.

### Missing reference errors

That is a normal CRAM requirement. Pass `-T reference.fa` or configure your standard CRAM reference environment.

## Next Steps

- adapt the example workflow in [examples/README.md](../examples/README.md)
- wire `CRAM2Excel` into a QC or reporting pipeline
- publish prebuilt binaries or package manager recipes

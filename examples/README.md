# Examples

## Tiny Demo

The repository includes a tiny reproducible example in [examples/tiny](tiny).

Files:

- [tiny.sam](tiny/tiny.sam)
- [tiny_ref.fa](tiny/tiny_ref.fa)
- [run_demo.sh](tiny/run_demo.sh)

Run it from the repository root:

```bash
./examples/tiny/run_demo.sh
```

That script:

1. builds the plugin if needed
2. checks that local SAMtools exists
3. generates `build/validation/tiny.cram`
4. exports `build/validation/tiny.xlsx`

## Real-World Pattern

For an existing CRAM:

```bash
HTS_PATH=$PWD/build \
./external/samtools-1.23/samtools view -h patient.cram -o cram2excel:patient.xlsx
```

For a CRAM that needs a reference:

```bash
HTS_PATH=$PWD/build \
./external/samtools-1.23/samtools view -h -T hg38.fa patient.cram -o cram2excel:patient.xlsx
```

## Output Expectations

- `Alignments1` contains the records
- `Header` contains the SAM header
- a valid workbook opens directly in Excel, Numbers, LibreOffice, or any OOXML-compatible spreadsheet tool

# Contributing

Thanks for helping improve `CRAM2Excel`.

## Development Flow

1. Build the plugin:

```bash
make
```

2. Build the local SAMtools/HTSlib toolchain if you do not already have it:

```bash
make toolchain
```

3. Run validation:

```bash
make validate
```

## Design Principles

- stay close to HTSlib's plugin ABI
- keep runtime dependencies minimal
- produce valid Excel files without external spreadsheet libraries
- keep exports predictable and easy to inspect

## Pull Request Checklist

- build succeeds on your platform
- `make validate` passes
- README and tutorial stay accurate
- examples still work

## Scope

Good contributions:

- safer workbook generation
- better worksheet metadata
- more example datasets
- packaging improvements
- documentation and tutorial improvements

Out of scope for now:

- formatting-heavy Excel styling
- formula generation
- arbitrary SAMtools subcommand plugins unrelated to export

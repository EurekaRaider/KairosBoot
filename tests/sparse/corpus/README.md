# Sparse fuzz seed corpus

Each `*.hex` file is a whitespace-separated hexadecimal byte stream. The
standalone parser-robustness test decodes it before passing the bytes to
`SparseImage::open`. The textual encoding keeps regression inputs reviewable
and portable without connecting sanitizer-only tests to the root Release build.

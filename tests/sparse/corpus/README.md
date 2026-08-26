# Sparse fuzz seed corpus

Each `*.hex` file is a whitespace-separated hexadecimal byte stream. A future
fuzz harness must decode it before passing the bytes to `SparseImage::open`.
The textual encoding keeps the seed definitions reviewable and portable while
the fuzz target is not yet connected to the root build.

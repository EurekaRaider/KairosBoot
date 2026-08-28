# Parser robustness corpus

All seeds are whitespace-separated hexadecimal bytes so their exact length and
contents remain reviewable. Sparse seeds live in `tests/sparse/corpus` and are
passed directly to `SparseImage::open` after decoding.

Response seeds begin with a two-byte little-endian configured response limit;
the remaining bytes are passed unchanged to `parse_response`. The standalone
test replays every committed seed before running bounded deterministic input
generation. It has no install rules and is not part of the product Release
build graph.

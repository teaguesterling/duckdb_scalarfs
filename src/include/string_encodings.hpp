#pragma once

#include "duckdb/common/string_util.hpp" // brings `string`

namespace duckdb {

// =============================================================================
// Shared string encode/decode helpers
// =============================================================================
// One home for the encodings scalarfs uses, so no filesystem/function
// re-implements them. Malformed input throws InvalidInputException.

// --- URL / percent coding (RFC 3986) ---------------------------------------
// Decode "%XX" escapes. Does NOT treat '+' as space, so it is the exact inverse
// of EncodeURLComponent (which emits "%20" for spaces).
string DecodeURLEncoded(const string &input);
// Percent-encode a URL component: unreserved chars (A-Za-z0-9-_.~) pass through,
// everything else becomes "%XX" (uppercase).
string EncodeURLComponent(const string &input);

// --- Backslash escape coding for arbitrary bytes ---------------------------
// Escapes "\\", "\n", "\r", "\t", "\0", and other control bytes as "\xNN".
// EncodeBlobEscapes adds NO protocol prefix (callers prepend e.g. "data+blob:").
string EncodeBlobEscapes(const string &input);
string DecodeBlobEscapes(const string &input);

} // namespace duckdb
